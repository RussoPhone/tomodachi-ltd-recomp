# Fontes de third_party/suyu/fonts

- `font_standard.ttf`, `font_korean.ttf`, `font_chinese_simplified.ttf`: Open Sans Regular, Apache-2.0.
- `font_chinese_traditional.ttf`: Open Sans Light, Apache-2.0.
- `font_extended_chinese_simplified.ttf`: **Open Sans Regular (cópia de `font_standard.ttf`, 883 codepoints)
  mais cinco símbolos acrescentados**: ♪ U+266A, ₩ U+20A9, △ U+25B3, ■ U+25A0, ◆ U+25C6 (levantamento em
  docs/research/2026-10-10-simbolos-usados-jogo.md). Nenhum glifo da Open Sans foi removido ou alterado
  (só a assinatura DSIG foi descartada). Os cinco contornos foram desenhados à mão por `make_font_extended.py`
  (fontTools, MIT). O arquivo é derivado da Open Sans e portanto **Apache-2.0** (texto em
  `LICENSE-Apache-2.0.txt`); os contornos dos cinco símbolos são do projeto nxrt, também redistribuíveis sob
  Apache-2.0 aqui.
  Aviso: Digitized data copyright © 2010-2011, Google Corporation. Licensed under the Apache License,
  Version 2.0 (http://www.apache.org/licenses/LICENSE-2.0). Este arquivo foi modificado: acréscimo dos cinco
  glifos acima, remoção da tabela DSIG.
  Esta slot **não usa mais a OFL-1.1** (a Source Sans Pro foi removida).
- `font_nintendo_extended.ttf` (slots 5 e 6 do pl:u, nintendo_ext_003): **fonte própria do nxrt, GPL-3.0-or-later**
  (família `NxrtIcons`, nomes e licença nas tabelas `name`). 24 ícones de botões em área privada
  (U+E0A0–E0A9 A B X Y L R ZL ZR SL SR vazados, U+E0B3/E0B4 + −, U+E0E0–E0E9 os mesmos preenchidos,
  U+E0EF/E0F0 + − preenchidos) mais NUL, CR e espaço; cmap, nomes de glifo, avanços (1024) e caixas
  iguais aos da fonte anterior (YuzuOSSExtension, removida: sem texto de licença). Todos os contornos
  são desenhados por `tools/fonts/make_nintendo_extended.py` (círculo, retângulo arredondado e letras
  poligonais; sem dados de terceiros). Regerar: `python3 tools/fonts/make_nintendo_extended.py
  third_party/suyu/fonts/font_nintendo_extended.ttf` (fontTools, MIT).
- `LICENSE-OFL-1.1.txt`: mantido só como registro histórico; nenhuma fonte atual do diretório usa a OFL.

Para regerar a fonte da slot:
`python3 make_font_extended.py font_standard.ttf font_extended_chinese_simplified.ttf`.
