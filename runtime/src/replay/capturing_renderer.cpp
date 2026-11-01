// SPDX-License-Identifier: GPL-3.0-or-later
#include "replay/capturing_renderer.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <limits>
#include <stdexcept>
#include <string_view>

#ifdef _WIN32
#include <windows.h>
#endif

#include "log_macros.h"
#include "memory.h"
#include "nvn/nvn_layer.h"

namespace nxrt::replay {
namespace {
bool ParseU64(std::string_view text, u64& value) {
    if(text.empty()) return false;
    const auto result=std::from_chars(text.data(),text.data()+text.size(),value);
    return result.ec==std::errc{}&&result.ptr==text.data()+text.size();
}
std::string UtcNow() {
    const auto now=std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm,&now);
#else
    gmtime_r(&now,&tm);
#endif
    char text[32]{};
    if(std::strftime(text,sizeof(text),"%Y-%m-%dT%H:%M:%SZ",&tm)==0) throw std::runtime_error("cannot format UTC date");
    return text;
}
std::filesystem::path CurrentExecutablePath(const std::filesystem::path& supplied) {
#ifdef _WIN32
    std::wstring buffer(32768,L'\0');
    const DWORD size=GetModuleFileNameW(nullptr,buffer.data(),static_cast<DWORD>(buffer.size()));
    if(size==0||size>=buffer.size()) throw std::runtime_error("cannot locate current executable");
    buffer.resize(size); return std::filesystem::path(buffer);
#elif defined(__linux__)
    std::error_code ec;
    auto path=std::filesystem::read_symlink("/proc/self/exe",ec);
    if(!ec) return path;
    return std::filesystem::absolute(supplied);
#else
    return std::filesystem::absolute(supplied);
#endif
}
}

std::optional<CaptureOptions> CaptureOptionsFromEnvironment(const std::filesystem::path& executable) {
    const char* frames=std::getenv("NXRT_CAPTURE_FRAMES");
    const char* directory=std::getenv("NXRT_CAPTURE_DIR");
    if(!frames&&!directory) return std::nullopt;
    if(!frames||!directory||!*directory) {
        LOG_ERROR("capture disabled: set both NXRT_CAPTURE_FRAMES and NXRT_CAPTURE_DIR");
        return std::nullopt;
    }
    const std::string_view setting(frames);
    const auto separator=setting.find(':');
    u64 first=0,count=0;
    if(separator==std::string_view::npos||setting.find(':',separator+1)!=std::string_view::npos||
       !ParseU64(setting.substr(0,separator),first)||!ParseU64(setting.substr(separator+1),count)||count==0||
       first>std::numeric_limits<u64>::max()-count) {
        LOG_ERROR("capture disabled: invalid NXRT_CAPTURE_FRAMES; expected <first>:<positive count>");
        return std::nullopt;
    }
    try {
        CaptureOptions options;
        options.first_frame=first; options.frame_count=count;
        options.directory=std::filesystem::absolute(directory);
        options.executable=CurrentExecutablePath(executable);
        return options;
    } catch(const std::exception& error) {
        LOG_ERROR("capture disabled while resolving paths: %s",error.what());
        return std::nullopt;
    }
}

CapturingRenderer::CapturingRenderer(nvn::Renderer& renderer, CaptureOptions options)
    : renderer_(renderer), options_(std::move(options)) {}
CapturingRenderer::~CapturingRenderer() {
    if(active_) {
        try { if(pages_) pages_->Close(); if(events_.is_open()) events_.close(); }
        catch(...) {}
        LOG_WARN("capture incomplete: captured %llu of %llu requested presents",
                 static_cast<unsigned long long>(recorded_presents_),
                 static_cast<unsigned long long>(options_.frame_count));
    }
}
void CapturingRenderer::Begin() {
    if(active_||finished_) return;
    if(std::filesystem::exists(options_.directory)&&!std::filesystem::is_empty(options_.directory))
        throw std::runtime_error("capture directory already contains files");
    std::filesystem::create_directories(options_.directory);
    events_.open(options_.directory/"events.bin",std::ios::binary|std::ios::trunc);
    if(!events_) throw std::runtime_error("cannot create events.bin");
    WriteEventsHeader(events_);
    pages_=std::make_unique<PageStore>(options_.directory);
    std::ofstream model_file(options_.directory/"model.bin",std::ios::binary|std::ios::trunc);
    if(!model_file) throw std::runtime_error("cannot create model.bin");
    WriteModel(model_file,nvn::GetModel());
    model_file.close();
    if(!model_file) throw std::runtime_error("failed writing model.bin");
    const auto digest=Sha256File(options_.executable);
    WriteManifest(options_.directory/"manifest.json",
                  CaptureManifest{3,options_.first_frame,options_.frame_count,UtcNow(),digest});
    active_=true;
    LOG_INFO("capture started at present frame %llu (%llu frames), directory=%s",
             static_cast<unsigned long long>(options_.first_frame),
             static_cast<unsigned long long>(options_.frame_count),options_.directory.string().c_str());
}
void CapturingRenderer::Finish() {
    if(!active_||finished_) return;
    events_.flush(); events_.close(); pages_->Close();
    finished_=true; active_=false;
    const double mb=static_cast<double>(pages_->unique_pages()*kCapturePageSize)/(1024.0*1024.0);
    LOG_INFO("capture: %llu eventos, %llu páginas únicas, %.2f MB, %llu páginas não mapeadas ignoradas",
             static_cast<unsigned long long>(event_count_),
             static_cast<unsigned long long>(pages_->unique_pages()),mb,
             static_cast<unsigned long long>(unmapped_pages_skipped_));
}
void CapturingRenderer::Fail(const char* message) {
    LOG_ERROR("capture stopped: %s",message);
    active_=false; finished_=true;
    try { if(events_.is_open()) { events_.flush(); events_.close(); } if(pages_) pages_->Close(); }
    catch(...) {}
}
void CapturingRenderer::Execute(const nvn::CommandList& list) {
    ExecuteImpl(list, nullptr);
}
void CapturingRenderer::ExecuteSubmission(const nvn::SubmissionSnapshot& submission) {
    ExecuteImpl(submission.list, &submission);
}
void CapturingRenderer::ExecuteImpl(const nvn::CommandList& list, const nvn::SubmissionSnapshot* submission) {
    std::lock_guard capture_guard(capture_lock_);
    auto& execution_model = submission && submission->execution_model ? *submission->execution_model : nvn::GetModel();
    if(!active_&&!finished_&&options_.ShouldStartOnFirstExecute()) {
        try { Begin(); }
        catch(const std::exception& error) { Fail(error.what()); }
    }
    if(active_) {
        try {
            CaptureEvent state; state.type=CaptureEvent::Type::State;
            state.driver_base=nvn::DriverModuleBase();
            state.model_snapshot=SnapshotExecutionModel(execution_model,list);
            WriteEvent(events_,state); ++event_count_;
            CaptureEvent event; event.type=CaptureEvent::Type::Execute; event.commands=list;
            std::vector<nvn::MemoryPool> pools;
            std::vector<nvn::GuestAddr> pool_handles;
            const auto descriptor_objects=SubmissionDescriptorPoolObjects(execution_model,list);
            {
                auto& model=execution_model;
                std::lock_guard guard(model.lock);
                pools.reserve(model.pools.size());
                pool_handles.reserve(model.pools.size());
                for(const auto& [address,pool]:model.pools) {
                    pools.push_back(pool);
                    if(pool.storage&&pool.size) pool_handles.push_back(address);
                }
            }
            for(const auto object:descriptor_objects) {
                if(g_memory.IsMapped(object,sizeof(u64))) {
                    const auto backing=g_memory.Read<u64>(object);
                    if(backing) pool_handles.push_back(backing);
                }
            }
            std::sort(pools.begin(),pools.end(),[](const auto& a,const auto& b){ return a.storage<b.storage; });
            std::array<std::byte,kCapturePageSize> bytes{};
            for(const auto& pool:pools) {
                if(pool.size==0) continue;
                if(pool.storage>std::numeric_limits<u64>::max()-pool.size) throw std::runtime_error("memory pool address overflow");
                const u64 begin=pool.storage&~(static_cast<u64>(kCapturePageSize)-1);
                const u64 end_unaligned=pool.storage+pool.size;
                if(end_unaligned>std::numeric_limits<u64>::max()-(kCapturePageSize-1)) throw std::runtime_error("memory pool end overflow");
                const u64 end=(end_unaligned+kCapturePageSize-1)&~(static_cast<u64>(kCapturePageSize)-1);
                for(u64 va=begin;va<end;va+=kCapturePageSize) {
                    if(!g_memory.IsMapped(va,kCapturePageSize)) { ++unmapped_pages_skipped_; continue; }
                    g_memory.ReadBlock(va,bytes.data(),bytes.size());
                    const auto id=pages_->Put(va,bytes);
                    event.pages.push_back({va,id});
                }
            }
            auto extra_pages=MemoryPoolObjectPages(pool_handles);
            for(const auto page:ExecutionCpuReadPages(execution_model,list)) {
                if(g_memory.IsMapped(page,kCapturePageSize)) extra_pages.push_back(page);
            }
            for(const auto page:DescriptorPoolObjectPages(descriptor_objects)) extra_pages.push_back(page);
            const auto offset=[](const char* name,u64 fallback) {
                const auto* text=std::getenv(name);
                return text&&*text?std::strtoull(text,nullptr,0):fallback;
            };
            const u64 format_offset=offset("NXRT_NVN_FORMAT_TABLE",0xb69208);
            const u64 storage_offset=offset("NXRT_NVN_SSBO_TABLE",0x915550);
            u64 format_table=0;
            if(state.driver_base) {
                if(state.driver_base>std::numeric_limits<u64>::max()-format_offset ||
                   !g_memory.IsMapped(state.driver_base+format_offset,sizeof(u64)))
                    throw std::runtime_error("driver format table pointer is not mapped");
                format_table=g_memory.Read<u64>(state.driver_base+format_offset);
            }
            for(const auto page:DriverTablePages(state.driver_base,format_offset,storage_offset,format_table)) {
                if(g_memory.IsMapped(page,kCapturePageSize)) extra_pages.push_back(page);
            }
            std::sort(extra_pages.begin(),extra_pages.end());
            extra_pages.erase(std::unique(extra_pages.begin(),extra_pages.end()),extra_pages.end());
            for(const u64 va:extra_pages) {
                if(!g_memory.IsMapped(va,kCapturePageSize))
                    throw std::runtime_error("memory pool object page is not mapped");
                g_memory.ReadBlock(va,bytes.data(),bytes.size());
                const auto id=pages_->Put(va,bytes);
                event.pages.push_back({va,id});
            }
            WriteEvent(events_,event); ++event_count_;
        } catch(const std::exception& error) { Fail(error.what()); }
    }
    if (submission) renderer_.ExecuteSubmission(*submission);
    else renderer_.Execute(list);
}
void CapturingRenderer::Present(nvn::GuestAddr texture,u64 frame) {
    std::lock_guard capture_guard(capture_lock_);
    const bool preroll=options_.ShouldStartBeforePresent(frame);
    if(!active_&&!finished_&&(preroll||options_.IncludesPresent(frame))) {
        try {
            Begin();
            if(active_&&preroll)
                LOG_INFO("capture armed at preceding present frame %llu for first captured frame %llu",
                         static_cast<unsigned long long>(frame),
                         static_cast<unsigned long long>(options_.first_frame));
        }
        catch(const std::exception& error) { Fail(error.what()); }
    }
    if(active_&&options_.IncludesPresent(frame)) {
        try {
            CaptureEvent event; event.type=CaptureEvent::Type::Present; event.texture=texture; event.frame=frame;
            WriteEvent(events_,event); ++event_count_; ++recorded_presents_;
        } catch(const std::exception& error) { Fail(error.what()); }
    }
    renderer_.Present(texture,frame);
    if(active_&&recorded_presents_>=options_.frame_count) Finish();
}

} // namespace nxrt::replay
