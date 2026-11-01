// SPDX-License-Identifier: GPL-3.0-or-later
#include "replay/capture_format.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <iomanip>
#include <functional>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>

namespace nxrt::replay {
namespace {
constexpr u64 kMaxElements = 1ULL << 24;
constexpr u64 kMaxStateBytes = 64ULL * 1024 * 1024;

void WriteBytes(std::ostream& out, const void* data, std::size_t size) {
    out.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    if (!out) throw std::runtime_error("capture write failed");
}
void ReadBytes(std::istream& in, void* data, std::size_t size) {
    in.read(static_cast<char*>(data), static_cast<std::streamsize>(size));
    if (!in) throw std::runtime_error("truncated or unreadable capture");
}

template<class T, bool = std::is_enum_v<T>> struct RawType { using type = T; };
template<class T> struct RawType<T, true> { using type = std::underlying_type_t<T>; };

template<class T> void WriteInt(std::ostream& out, T value) {
    static_assert(std::is_integral_v<T> || std::is_enum_v<T>);
    using R = typename RawType<T>::type;
    using U = std::make_unsigned_t<R>;
    U bits = static_cast<U>(value);
    std::array<char, sizeof(U)> bytes{};
    for (std::size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<char>((bits >> (i * 8)) & 0xff);
    WriteBytes(out, bytes.data(), bytes.size());
}
template<class T> T ReadInt(std::istream& in) {
    static_assert(std::is_integral_v<T> || std::is_enum_v<T>);
    using R = typename RawType<T>::type;
    using U = std::make_unsigned_t<R>;
    std::array<unsigned char, sizeof(U)> bytes{};
    ReadBytes(in, bytes.data(), bytes.size());
    U bits = 0;
    for (std::size_t i = 0; i < bytes.size(); ++i) bits |= static_cast<U>(bytes[i]) << (i * 8);
    return static_cast<T>(bits);
}
void WriteFloat(std::ostream& out, float value) { WriteInt(out, std::bit_cast<u32>(value)); }
float ReadFloat(std::istream& in) { return std::bit_cast<float>(ReadInt<u32>(in)); }

u64 ReadCount(std::istream& in) {
    const u64 count = ReadInt<u64>(in);
    if (count > kMaxElements) throw std::runtime_error("capture collection exceeds safety limit");
    return count;
}
template<class Map> std::vector<typename Map::key_type> SortedKeys(const Map& map) {
    std::vector<typename Map::key_type> keys;
    keys.reserve(map.size());
    for (const auto& [key, value] : map) { (void)value; keys.push_back(key); }
    std::sort(keys.begin(), keys.end());
    return keys;
}

void WriteMemoryPool(std::ostream& o, const nvn::MemoryPool& v) {
    WriteInt(o,v.storage); WriteInt(o,v.size); WriteInt(o,v.flags); WriteInt(o,v.gpu); WriteInt(o,v.gpu2);
    WriteInt(o,v.generation); WriteInt(o,static_cast<u8>(v.is_virtual));
}
nvn::MemoryPool ReadMemoryPool(std::istream& i) {
    nvn::MemoryPool v; v.storage=ReadInt<u64>(i); v.size=ReadInt<u64>(i); v.flags=ReadInt<u32>(i);
    v.gpu=ReadInt<u64>(i); v.gpu2=ReadInt<u64>(i); v.generation=ReadInt<u64>(i); v.is_virtual=ReadInt<u8>(i)!=0; return v;
}
void WriteVirtualMapping(std::ostream& o, const nvn::VirtualMapping& v) {
    WriteInt(o,v.size); WriteInt(o,v.cpu); WriteInt(o,v.virtual_pool); WriteInt(o,v.virtual_offset);
    WriteInt(o,v.physical_pool); WriteInt(o,v.physical_generation); WriteInt(o,v.physical_offset);
}
nvn::VirtualMapping ReadVirtualMapping(std::istream& i) {
    return {ReadInt<u64>(i),ReadInt<u64>(i),ReadInt<u64>(i),ReadInt<u64>(i),ReadInt<u64>(i),ReadInt<u64>(i),ReadInt<u64>(i)};
}
void WriteTextureDesc(std::ostream& o, const nvn::TextureDesc& v) {
    WriteInt(o,v.flags); WriteInt(o,v.target); WriteInt(o,v.width); WriteInt(o,v.height); WriteInt(o,v.depth);
    WriteInt(o,v.levels); WriteInt(o,v.format); WriteInt(o,v.samples);
    for (auto x:v.swizzle) WriteInt(o,x);
    WriteInt(o,v.depth_stencil_mode); WriteInt(o,v.stride); WriteInt(o,v.pool); WriteInt(o,v.offset); WriteInt(o,v.packaged_data);
}
nvn::TextureDesc ReadTextureDesc(std::istream& i) {
    nvn::TextureDesc v; v.flags=ReadInt<u32>(i); v.target=ReadInt<u32>(i); v.width=ReadInt<u32>(i); v.height=ReadInt<u32>(i);
    v.depth=ReadInt<u32>(i); v.levels=ReadInt<u32>(i); v.format=ReadInt<u32>(i); v.samples=ReadInt<u32>(i);
    for (auto& x:v.swizzle) x=ReadInt<u32>(i);
    v.depth_stencil_mode=ReadInt<u32>(i); v.stride=ReadInt<u32>(i); v.pool=ReadInt<u64>(i); v.offset=ReadInt<u64>(i);
    v.packaged_data=ReadInt<u64>(i); return v;
}
void WriteTextureView(std::ostream& o, const nvn::TextureView& v) {
    WriteInt(o,v.base_level); WriteInt(o,v.num_levels); WriteInt(o,v.min_layer); WriteInt(o,v.num_layers); WriteInt(o,v.format);
}
nvn::TextureView ReadTextureView(std::istream& i, bool has_format) {
    nvn::TextureView view{ReadInt<u32>(i),ReadInt<u32>(i),ReadInt<u32>(i),ReadInt<u32>(i)};
    if (has_format) view.format = ReadInt<u32>(i);
    return view;
}
void WriteProgram(std::ostream& o, const nvn::Program& v) {
    WriteInt(o,static_cast<u64>(v.stages.size()));
    for (const auto& stage:v.stages) { WriteInt(o,stage.code); WriteInt(o,stage.control); }
    WriteInt(o,v.serial);
}
nvn::Program ReadProgram(std::istream& i) {
    nvn::Program v; const auto count=ReadCount(i); v.stages.resize(static_cast<std::size_t>(count));
    for (auto& stage:v.stages) { stage.code=ReadInt<u64>(i); stage.control=ReadInt<u64>(i); }
    v.serial=ReadInt<u64>(i); return v;
}

// SHA-256, kept local so capture creation has no dependency on host crypto packages.
constexpr std::array<u32,64> kShaK{
  0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
  0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
  0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
  0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
  0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
  0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
  0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
  0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
std::array<std::byte,32> Sha256(std::span<const std::byte> input) {
    std::vector<u8> data; data.reserve(input.size()+72);
    for (auto b:input) data.push_back(std::to_integer<u8>(b));
    const u64 bit_length=static_cast<u64>(data.size())*8; data.push_back(0x80);
    while ((data.size()%64)!=56) data.push_back(0);
    for (int shift=56;shift>=0;shift-=8) data.push_back(static_cast<u8>(bit_length>>shift));
    std::array<u32,8> h{0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    for (std::size_t off=0;off<data.size();off+=64) {
        std::array<u32,64> w{};
        for (std::size_t n=0;n<16;++n) w[n]=(u32(data[off+4*n])<<24)|(u32(data[off+4*n+1])<<16)|(u32(data[off+4*n+2])<<8)|u32(data[off+4*n+3]);
        for (std::size_t n=16;n<64;++n) { const u32 a=std::rotr(w[n-15],7)^std::rotr(w[n-15],18)^(w[n-15]>>3); const u32 b=std::rotr(w[n-2],17)^std::rotr(w[n-2],19)^(w[n-2]>>10); w[n]=w[n-16]+a+w[n-7]+b; }
        u32 a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],z=h[7];
        for (std::size_t n=0;n<64;++n) { const u32 s1=std::rotr(e,6)^std::rotr(e,11)^std::rotr(e,25); const u32 ch=(e&f)^(~e&g); const u32 t1=z+s1+ch+kShaK[n]+w[n]; const u32 s0=std::rotr(a,2)^std::rotr(a,13)^std::rotr(a,22); const u32 maj=(a&b)^(a&c)^(b&c); const u32 t2=s0+maj; z=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2; }
        h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;h[5]+=f;h[6]+=g;h[7]+=z;
    }
    std::array<std::byte,32> out{};
    for (std::size_t n=0;n<8;++n) for (std::size_t j=0;j<4;++j) out[4*n+j]=std::byte((h[n]>>(24-8*j))&0xff);
    return out;
}
std::string Hex(std::span<const std::byte> bytes) {
    std::ostringstream out; out << std::hex << std::setfill('0');
    for (auto b:bytes) out << std::setw(2) << std::to_integer<unsigned>(b);
    return out.str();
}
std::string JsonEscape(const std::string& value) {
    std::string out; out.reserve(value.size()+2);
    for (unsigned char c:value) {
        if (c=='"' || c=='\\') { out.push_back('\\'); out.push_back(static_cast<char>(c)); }
        else if (c=='\n') out += "\\n"; else if (c=='\r') out += "\\r"; else if (c=='\t') out += "\\t";
        else if (c<0x20) { const char* hex="0123456789abcdef"; out += "\\u00"; out.push_back(hex[c>>4]); out.push_back(hex[c&15]); }
        else out.push_back(static_cast<char>(c));
    }
    return out;
}
}

void WriteCommand(std::ostream& out, const nvn::Command& command) {
    WriteInt(out,command.op); for (auto x:command.a) WriteInt(out,x); for (auto x:command.f) WriteFloat(out,x);
    WriteInt(out,static_cast<u64>(command.words.size())); for (auto x:command.words) WriteInt(out,x);
}
nvn::Command ReadCommand(std::istream& in) {
    nvn::Command command{}; command.op=ReadInt<nvn::Op>(in);
    if (static_cast<u8>(command.op)>static_cast<u8>(nvn::Op::CopyTextureToBuffer)) throw std::runtime_error("invalid command opcode");
    for (auto& x:command.a) x=ReadInt<u64>(in); for (auto& x:command.f) x=ReadFloat(in);
    const auto count=ReadCount(in); command.words.resize(static_cast<std::size_t>(count));
    for (auto& x:command.words) x=ReadInt<u32>(in); return command;
}
void WriteCommandList(std::ostream& out, const nvn::CommandList& list) {
    WriteInt(out,static_cast<u64>(list.commands.size())); for (const auto& command:list.commands) WriteCommand(out,command);
}
nvn::CommandList ReadCommandList(std::istream& in) {
    nvn::CommandList list; const auto count=ReadCount(in); list.commands.reserve(static_cast<std::size_t>(count));
    for (u64 n=0;n<count;++n) list.commands.push_back(ReadCommand(in)); return list;
}

std::vector<u64> SubmissionDescriptorPoolObjects(nvn::Model& model, const nvn::CommandList& submitted) {
    std::vector<u64> objects;
    std::lock_guard guard(model.lock);
    for (const auto& [object,entries] : model.texture_pools) { (void)entries; if (object) objects.push_back(object); }
    for (const auto& [object,entries] : model.sampler_pools) { (void)entries; if (object) objects.push_back(object); }
    std::function<void(const nvn::CommandList&, unsigned)> visit;
    visit = [&](const nvn::CommandList& list, unsigned depth) {
        for (const auto& command : list.commands) {
            if ((command.op == nvn::Op::SetTexturePool || command.op == nvn::Op::SetSamplerPool) && command.a[0])
                objects.push_back(command.a[0]);
            else if (command.op == nvn::Op::CallCommands && depth < 4) {
                for (std::size_t i = 0; i + 1 < command.words.size(); i += 2) {
                    const u64 handle = command.words[i] | (static_cast<u64>(command.words[i+1]) << 32);
                    const auto found = model.finished.find(handle);
                    if (found == model.finished.end() || !found->second)
                        throw std::runtime_error("missing nested list in descriptor pool scan");
                    visit(*found->second, depth + 1);
                }
            }
        }
    };
    visit(submitted, 0);
    std::sort(objects.begin(), objects.end());
    objects.erase(std::unique(objects.begin(), objects.end()), objects.end());
    return objects;
}

std::vector<u64> ExecutionCpuReadPages(nvn::Model& model, const nvn::CommandList& submitted) {
    std::vector<u64> pages;
    const auto add = [&](u64 address, u64 length) {
        if (!address || !length) return;
        if (address > std::numeric_limits<u64>::max() - (length - 1))
            throw std::overflow_error("CPU dependency range overflow");
        const u64 last = (address + length - 1) & ~u64{kCapturePageSize - 1};
        for (u64 page = address & ~u64{kCapturePageSize - 1};; page += kCapturePageSize) {
            pages.push_back(page);
            if (page == last) break;
        }
    };
    std::lock_guard guard(model.lock);
    for (const auto& [address,program] : model.programs) {
        (void)address;
        for (const auto& stage : program.stages) add(stage.control, 0x800);
    }
    std::function<void(const nvn::CommandList&, unsigned)> visit;
    visit = [&](const nvn::CommandList& list, unsigned depth) {
        for (const auto& command : list.commands) {
            if (command.op == nvn::Op::BindTexturesDeferred && !command.a[4]) {
                if (command.a[2] > 32) throw std::runtime_error("deferred texture array exceeds slot count");
                add(command.a[3], command.a[2] * 8);
            } else if (command.op == nvn::Op::CallCommands && depth < 4) {
                for (std::size_t i = 0; i + 1 < command.words.size(); i += 2) {
                    const u64 handle = command.words[i] | (static_cast<u64>(command.words[i+1]) << 32);
                    const auto found = model.finished.find(handle);
                    if (found == model.finished.end() || !found->second)
                        throw std::runtime_error("missing nested command list in CPU dependency scan");
                    visit(*found->second, depth + 1);
                }
            }
        }
    };
    visit(submitted, 0);
    std::sort(pages.begin(), pages.end());
    pages.erase(std::unique(pages.begin(), pages.end()), pages.end());
    return pages;
}

std::vector<u64> DescriptorPoolObjectPages(std::span<const u64> objects) {
    std::vector<u64> pages;
    for (const u64 object : objects) {
        if (!object) continue;
        if (object > std::numeric_limits<u64>::max() - 19)
            throw std::overflow_error("descriptor pool object overflow");
        pages.push_back(object & ~u64{kCapturePageSize - 1});
        pages.push_back((object + 19) & ~u64{kCapturePageSize - 1});
    }
    std::sort(pages.begin(), pages.end());
    pages.erase(std::unique(pages.begin(), pages.end()), pages.end());
    return pages;
}

std::vector<u64> DriverTablePages(u64 base, u64 format_offset, u64 storage_offset, u64 format_table) {
    std::vector<u64> result;
    const auto add_range = [&](u64 start, u64 length) {
        if (start > std::numeric_limits<u64>::max() - length)
            throw std::overflow_error("driver table address overflow");
        const u64 last = (start + length - 1) & ~u64{kCapturePageSize - 1};
        for (u64 page = start & ~u64{kCapturePageSize - 1};; page += kCapturePageSize) {
            result.push_back(page);
            if (page == last) break;
        }
    };
    if (base) {
        if (base > std::numeric_limits<u64>::max() - std::max(format_offset, storage_offset))
            throw std::overflow_error("driver base address overflow");
        add_range(base + format_offset, 8);
        add_range(base + storage_offset, 6 * 4);
        // NVN vertex attributes encode an 11-bit format index (word >> 21).
        if (format_table) add_range(format_table, 2048 * 0x30);
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

std::vector<u64> MemoryPoolObjectPages(std::span<const nvn::GuestAddr> pool_handles) {
    std::vector<u64> pages;
    pages.reserve(pool_handles.size() * 2);
    const auto add_field_pages = [&pages](u64 address) {
        if (address > std::numeric_limits<u64>::max() - sizeof(u64))
            throw std::overflow_error("memory pool object address overflow");
        const u64 first = address & ~(static_cast<u64>(kCapturePageSize) - 1);
        const u64 last = (address + sizeof(u64) - 1) & ~(static_cast<u64>(kCapturePageSize) - 1);
        pages.push_back(first);
        if (last != first) pages.push_back(last);
    };
    for (const auto handle : pool_handles) {
        if (handle > std::numeric_limits<u64>::max() - 0x20)
            throw std::overflow_error("memory pool handle overflow");
        add_field_pages(handle + 0x18);
        add_field_pages(handle + 0x20);
        if (handle > std::numeric_limits<u64>::max() - 0x30)
            throw std::overflow_error("memory pool storage pointer overflow");
        add_field_pages(handle + 0x30);
    }
    std::sort(pages.begin(), pages.end());
    pages.erase(std::unique(pages.begin(), pages.end()), pages.end());
    return pages;
}

void WriteEventsHeader(std::ostream& out) { WriteBytes(out,"NXEVT003",8); }
u32 ReadEventsHeader(std::istream& in) {
    std::array<char,8> magic{}; ReadBytes(in,magic.data(),magic.size());
    if (std::memcmp(magic.data(), "NXEVT001", 8) == 0) return 1;
    if (std::memcmp(magic.data(), "NXEVT002", 8) == 0) return 2;
    if (std::memcmp(magic.data(), "NXEVT003", 8) == 0) return 3;
    throw std::runtime_error("invalid events header");
}
void WriteEvent(std::ostream& out, const CaptureEvent& event) {
    WriteInt(out,event.type);
    if(event.type==CaptureEvent::Type::Execute) {
        if (!event.commands.origins.empty() && event.commands.origins.size() != event.commands.commands.size())
            throw std::invalid_argument("command origin count does not match command count");
        WriteCommandList(out,event.commands); WriteInt(out,static_cast<u64>(event.pages.size()));
        for(const auto& page:event.pages) { WriteInt(out,page.va); WriteInt(out,page.id); }
        WriteInt(out,static_cast<u64>(event.commands.origins.size()));
        for (const auto& origin : event.commands.origins) {
            WriteInt(out, origin.list_handle);
            WriteInt(out, origin.depth);
        }
    } else if(event.type==CaptureEvent::Type::Present) {
        WriteInt(out,event.texture); WriteInt(out,event.frame);
    } else if(event.type==CaptureEvent::Type::State) {
        if(event.model_snapshot.size()>kMaxStateBytes) throw std::runtime_error("state snapshot exceeds 64 MiB limit");
        WriteInt(out,event.driver_base);
        WriteInt(out,static_cast<u64>(event.model_snapshot.size()));
        WriteBytes(out,event.model_snapshot.data(),event.model_snapshot.size());
    } else throw std::invalid_argument("invalid capture event type");
}
CaptureEvent ReadEvent(std::istream& in, u32 version) {
    CaptureEvent event; event.type=ReadInt<CaptureEvent::Type>(in);
    if(event.type==CaptureEvent::Type::Execute) {
        event.commands=ReadCommandList(in); const auto count=ReadCount(in); event.pages.resize(static_cast<std::size_t>(count));
        for(auto& page:event.pages) { page.va=ReadInt<u64>(in); page.id=ReadInt<u64>(in); }
        if (version >= 3) {
            const auto origin_count = ReadCount(in);
            if (origin_count != 0 && origin_count != event.commands.commands.size())
                throw std::runtime_error("capture command origin count does not match command count");
            event.commands.origins.resize(static_cast<std::size_t>(origin_count));
            for (auto& origin : event.commands.origins) {
                origin.list_handle = ReadInt<u64>(in);
                origin.depth = ReadInt<u32>(in);
            }
        }
    } else if(event.type==CaptureEvent::Type::Present) {
        event.texture=ReadInt<u64>(in); event.frame=ReadInt<u64>(in);
    } else if(event.type==CaptureEvent::Type::State) {
        event.driver_base=ReadInt<u64>(in);
        const auto size=ReadInt<u64>(in);
        if(size>kMaxStateBytes) throw std::runtime_error("state snapshot exceeds 64 MiB limit");
        event.model_snapshot.resize(static_cast<std::size_t>(size));
        ReadBytes(in,event.model_snapshot.data(),event.model_snapshot.size());
    } else throw std::runtime_error("invalid capture event type");
    return event;
}

void WriteModel(std::ostream& out, nvn::Model& model) {
    std::lock_guard guard(model.lock);
    WriteBytes(out,"NXMDL002",8); WriteInt(out,model.pool_generation);
    auto keys=SortedKeys(model.pools); WriteInt(out,static_cast<u64>(keys.size())); for (auto k:keys) { WriteInt(out,k); WriteMemoryPool(out,model.pools.at(k)); }
    keys=SortedKeys(model.virtual_mappings); WriteInt(out,static_cast<u64>(keys.size())); for (auto k:keys) { WriteInt(out,k); WriteVirtualMapping(out,model.virtual_mappings.at(k)); }
    keys=SortedKeys(model.textures); WriteInt(out,static_cast<u64>(keys.size())); for (auto k:keys) { WriteInt(out,k); WriteTextureDesc(out,model.textures.at(k).desc); WriteInt(out,model.textures.at(k).serial); }
    keys=SortedKeys(model.programs); WriteInt(out,static_cast<u64>(keys.size())); for (auto k:keys) { WriteInt(out,k); WriteProgram(out,model.programs.at(k)); }
    keys=SortedKeys(model.texture_pools); WriteInt(out,static_cast<u64>(keys.size()));
    for (auto k:keys) { const auto& inner=model.texture_pools.at(k); WriteInt(out,k); auto ids=SortedKeys(inner); WriteInt(out,static_cast<u64>(ids.size())); for (auto id:ids) { const auto pair=inner.at(id); WriteInt(out,id); WriteInt(out,pair.first); WriteInt(out,pair.second); } }
    keys=SortedKeys(model.texture_pool_views); WriteInt(out,static_cast<u64>(keys.size()));
    for (auto k:keys) { const auto& inner=model.texture_pool_views.at(k); WriteInt(out,k); auto ids=SortedKeys(inner); WriteInt(out,static_cast<u64>(ids.size())); for (auto id:ids) { WriteInt(out,id); WriteTextureView(out,inner.at(id)); } }
    keys=SortedKeys(model.windows); WriteInt(out,static_cast<u64>(keys.size()));
    for (auto k:keys) { const auto& window=model.windows.at(k); WriteInt(out,k); WriteInt(out,static_cast<u64>(window.textures.size())); for(auto t:window.textures) WriteInt(out,t); WriteInt(out,window.acquired); }
    keys=SortedKeys(model.finished); WriteInt(out,static_cast<u64>(keys.size())); for (auto k:keys) { WriteInt(out,k); WriteCommandList(out,*model.finished.at(k)); }
    WriteInt(out,model.texture_pool_registration.new_entries); WriteInt(out,model.texture_pool_registration.changed_entries); WriteInt(out,model.texture_pool_registration.unchanged_entries);
}
std::unique_ptr<nvn::Model> ReadModel(std::istream& in) {
    std::array<char,8> magic{}; ReadBytes(in,magic.data(),magic.size());
    const bool has_view_format = std::memcmp(magic.data(), "NXMDL002", 8) == 0;
    if (!has_view_format && std::memcmp(magic.data(), "NXMDL001", 8) != 0)
        throw std::runtime_error("invalid model snapshot header");
    auto model=std::make_unique<nvn::Model>(); model->pool_generation=ReadInt<u64>(in);
    auto count=ReadCount(in); for(u64 n=0;n<count;++n) { auto k=ReadInt<u64>(in); model->pools.emplace(k,ReadMemoryPool(in)); }
    count=ReadCount(in); for(u64 n=0;n<count;++n) { auto k=ReadInt<u64>(in); model->virtual_mappings.emplace(k,ReadVirtualMapping(in)); }
    count=ReadCount(in); for(u64 n=0;n<count;++n) { auto k=ReadInt<u64>(in); model->textures.emplace(k,nvn::Texture{ReadTextureDesc(in),ReadInt<u64>(in)}); }
    count=ReadCount(in); for(u64 n=0;n<count;++n) { auto k=ReadInt<u64>(in); model->programs.emplace(k,ReadProgram(in)); }
    count=ReadCount(in); for(u64 n=0;n<count;++n) { auto pool=ReadInt<u64>(in); auto entries=ReadCount(in); auto& inner=model->texture_pools[pool]; for(u64 j=0;j<entries;++j) { auto id=ReadInt<u32>(in); auto texture=ReadInt<u64>(in); auto sampler=ReadInt<u64>(in); inner.emplace(id,std::pair{texture,sampler}); } }
    count=ReadCount(in); for(u64 n=0;n<count;++n) { auto pool=ReadInt<u64>(in); auto entries=ReadCount(in); auto& inner=model->texture_pool_views[pool]; for(u64 j=0;j<entries;++j) { auto id=ReadInt<u32>(in); inner.emplace(id,ReadTextureView(in, has_view_format)); } }
    count=ReadCount(in); for(u64 n=0;n<count;++n) { auto k=ReadInt<u64>(in); nvn::Window v; auto textures=ReadCount(in); v.textures.resize(static_cast<std::size_t>(textures)); for(auto& t:v.textures) t=ReadInt<u64>(in); v.acquired=ReadInt<u32>(in); model->windows.emplace(k,std::move(v)); }
    count=ReadCount(in); for(u64 n=0;n<count;++n) { auto k=ReadInt<u64>(in); model->finished.emplace(k,std::make_shared<nvn::CommandList>(ReadCommandList(in))); }
    model->texture_pool_registration={ReadInt<u64>(in),ReadInt<u64>(in),ReadInt<u64>(in)};
    return model;
}

void ReadModel(std::istream& in, nvn::Model& destination) {
    auto source = ReadModel(in);
    std::lock_guard guard(destination.lock);
    destination.pools = std::move(source->pools);
    destination.virtual_mappings = std::move(source->virtual_mappings);
    destination.textures = std::move(source->textures);
    destination.programs = std::move(source->programs);
    destination.texture_pools = std::move(source->texture_pools);
    destination.texture_pool_views = std::move(source->texture_pool_views);
    destination.windows = std::move(source->windows);
    destination.finished = std::move(source->finished);
    destination.texture_pool_registration = source->texture_pool_registration;
    destination.pool_generation = source->pool_generation;
}

std::string SnapshotExecutionModel(nvn::Model& live, const nvn::CommandList& submitted) {
    nvn::Model snapshot;
    {
        std::lock_guard guard(live.lock);
        snapshot.pools = live.pools;
        snapshot.virtual_mappings = live.virtual_mappings;
        snapshot.textures = live.textures;
        snapshot.programs = live.programs;
        snapshot.texture_pools = live.texture_pools;
        snapshot.texture_pool_views = live.texture_pool_views;
        snapshot.windows = live.windows;
        snapshot.texture_pool_registration = live.texture_pool_registration;
        snapshot.pool_generation = live.pool_generation;
        std::function<void(const nvn::CommandList&, unsigned)> visit;
        visit = [&](const nvn::CommandList& list, unsigned depth) {
            for (const auto& command : list.commands) {
                if (command.op != nvn::Op::CallCommands || depth >= 4) continue;
                if (command.words.size() % 2 != 0) throw std::runtime_error("invalid nested command handles");
                for (std::size_t i = 0; i < command.words.size(); i += 2) {
                    const u64 handle = command.words[i] | (static_cast<u64>(command.words[i+1]) << 32);
                    const auto found = live.finished.find(handle);
                    if (found == live.finished.end() || !found->second)
                        throw std::runtime_error("missing nested command list during capture");
                    snapshot.finished[handle] = found->second;
                    visit(*found->second, depth + 1);
                }
            }
        };
        visit(submitted, 0);
    }
    std::ostringstream bytes(std::ios::binary);
    WriteModel(bytes, snapshot);
    return bytes.str();
}

PageStore::PageStore(const std::filesystem::path& directory) : directory_(directory) {
    std::filesystem::create_directories(directory_);
    pages_.open(directory_/"pages.bin",std::ios::binary|std::ios::in|std::ios::out|std::ios::trunc);
    index_.open(directory_/"pages.idx",std::ios::binary|std::ios::trunc);
    if(!pages_||!index_) throw std::runtime_error("cannot create capture page files");
    index_.write("NXPGIDX1",8);
}
PageStore::~PageStore() { try { Close(); } catch (...) {} }
u64 PageStore::Put(u64 va, std::span<const std::byte> bytes) {
    if(closed_) throw std::runtime_error("page store is closed");
    if(bytes.size()!=kCapturePageSize) throw std::invalid_argument("capture pages must be exactly 4096 bytes");
    const auto hash=Sha256(bytes); const auto key=Hex(hash);
    auto it=hash_ids_.find(key); u64 id;
    if(it==hash_ids_.end()) {
        if(unique_pages_ >= kCaptureMaxPagesBytes / kCapturePageSize)
            throw std::runtime_error("capture pages exceeded the 10 GiB limit");
        id=unique_pages_+1; pages_.seekp(0,std::ios::end); const auto offset=static_cast<u64>(pages_.tellp());
        WriteBytes(pages_,bytes.data(),bytes.size()); pages_.flush();
        hashes_.push_back(hash); offsets_.push_back(offset); hash_ids_.emplace(key,id); unique_pages_=id;
        index_.put(1); WriteInt(index_,id); WriteInt(index_,offset); WriteInt(index_,static_cast<u32>(bytes.size()));
        WriteBytes(index_,hash.data(),hash.size());
    } else id=it->second;
    PageRef ref{va,id};
    if(reference_set_.insert(ref).second) {
        references_.push_back(ref); index_.put(2); WriteInt(index_,va); WriteInt(index_,id);
    }
    if(!pages_||!index_) throw std::runtime_error("capture page write failed");
    return id;
}
std::vector<std::byte> PageStore::Get(u64 id) {
    if(id==0||id>unique_pages_) throw std::out_of_range("unknown capture page id");
    std::vector<std::byte> bytes(kCapturePageSize); pages_.flush(); pages_.clear(); pages_.seekg(static_cast<std::streamoff>(offsets_[id-1]));
    ReadBytes(pages_,bytes.data(),bytes.size()); return bytes;
}
void PageStore::Close() {
    if(closed_) return; pages_.flush(); index_.flush(); pages_.close(); index_.close(); closed_=true;
}

CapturePageReader::CapturePageReader(const std::filesystem::path& directory)
    : pages_(directory / "pages.bin", std::ios::binary) {
    if (!pages_) throw std::runtime_error("cannot open pages.bin");
    const auto page_file_size = std::filesystem::file_size(directory / "pages.bin");
    if (page_file_size > kCaptureMaxPagesBytes || page_file_size % kCapturePageSize != 0)
        throw std::runtime_error("invalid pages.bin size");

    std::ifstream index(directory / "pages.idx", std::ios::binary);
    if (!index) throw std::runtime_error("cannot open pages.idx");
    std::array<char, 8> magic{};
    ReadBytes(index, magic.data(), magic.size());
    if (std::memcmp(magic.data(), "NXPGIDX1", magic.size()) != 0)
        throw std::runtime_error("invalid page index header");

    while (true) {
        const int raw_type = index.get();
        if (raw_type == std::char_traits<char>::eof()) {
            if (!index.eof()) throw std::runtime_error("failed reading pages.idx");
            break;
        }
        if (raw_type == 1) {
            const u64 id = ReadInt<u64>(index);
            const u64 offset = ReadInt<u64>(index);
            const u32 size = ReadInt<u32>(index);
            std::array<std::byte, 32> hash{};
            ReadBytes(index, hash.data(), hash.size());
            if (id != offsets_.size() + 1 || size != kCapturePageSize ||
                offset > page_file_size || size > page_file_size - offset || offset % kCapturePageSize != 0)
                throw std::runtime_error("invalid page index record");
            offsets_.push_back(offset);
            hashes_.push_back(hash);
        } else if (raw_type == 2) {
            const u64 va = ReadInt<u64>(index);
            const u64 id = ReadInt<u64>(index);
            if ((va & (kCapturePageSize - 1)) != 0 || id == 0)
                throw std::runtime_error("invalid page address reference");
            references_.push_back({va, id});
        } else {
            throw std::runtime_error("unknown page index record");
        }
    }
    if (offsets_.size() != page_file_size / kCapturePageSize)
        throw std::runtime_error("page index and pages.bin disagree");
    for (const auto& ref : references_) {
        if (ref.id > offsets_.size()) throw std::runtime_error("page reference names an unknown page id");
    }

    // Keep ordinary captures resident for fast replay. Larger captures remain
    // seekable so the reader does not reserve multiple gigabytes of RAM.
    constexpr u64 kResidentLimit = 1024ULL * 1024 * 1024;
    if (page_file_size <= kResidentLimit) {
        resident_pages_.resize(static_cast<std::size_t>(page_file_size));
        pages_.read(reinterpret_cast<char*>(resident_pages_.data()),
                    static_cast<std::streamsize>(resident_pages_.size()));
        if (!pages_) throw std::runtime_error("truncated pages.bin");
        for (std::size_t i = 0; i < offsets_.size(); ++i) {
            const auto bytes = std::span<const std::byte>(resident_pages_).subspan(
                static_cast<std::size_t>(offsets_[i]), kCapturePageSize);
            if (Sha256(bytes) != hashes_[i]) throw std::runtime_error("page hash mismatch");
        }
    }
}

std::span<const std::byte> CapturePageReader::Read(u64 id) {
    if (id == 0 || id > offsets_.size()) throw std::out_of_range("unknown capture page id");
    const std::size_t index = static_cast<std::size_t>(id - 1);
    if (!resident_pages_.empty()) {
        return std::span<const std::byte>(resident_pages_).subspan(
            static_cast<std::size_t>(offsets_[index]), kCapturePageSize);
    }
    pages_.clear();
    pages_.seekg(static_cast<std::streamoff>(offsets_[index]));
    ReadBytes(pages_, scratch_.data(), scratch_.size());
    if (Sha256(scratch_) != hashes_[index]) throw std::runtime_error("page hash mismatch");
    return scratch_;
}

std::string Sha256File(const std::filesystem::path& file) {
    std::ifstream in(file,std::ios::binary); if(!in) throw std::runtime_error("cannot open executable for SHA-256");
    std::vector<std::byte> bytes; std::array<char,64*1024> chunk{};
    while(in) { in.read(chunk.data(),chunk.size()); const auto n=in.gcount(); const auto* p=reinterpret_cast<const std::byte*>(chunk.data()); bytes.insert(bytes.end(),p,p+n); }
    if(!in.eof()) throw std::runtime_error("failed reading executable for SHA-256");
    return Hex(Sha256(bytes));
}
void WriteManifest(const std::filesystem::path& file, const CaptureManifest& manifest) {
    std::ofstream out(file,std::ios::binary|std::ios::trunc); if(!out) throw std::runtime_error("cannot create capture manifest");
    out << "{\n  \"version\": " << manifest.version << ",\n  \"frames\": {\"first\": " << manifest.first_frame
        << ", \"count\": " << manifest.frame_count << "},\n  \"date\": \"" << JsonEscape(manifest.date)
        << "\",\n  \"executable_sha256\": \"" << JsonEscape(manifest.executable_sha256) << "\"\n}\n";
    if(!out) throw std::runtime_error("failed writing capture manifest");
}

} // namespace nxrt::replay
