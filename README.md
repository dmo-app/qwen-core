# Core

> **DIRECÇÃO ACTUAL: PATTERN MAKER + TOOLS.**
> Manter catálogo de padrões e alinhamento kick/baixo/pratos. Tools agrupam plugins externos em chains, com parâmetros aplicados a pistas/partes ou compassos inicial/final.
> Tools: explorar painéis simplificados, com acesso directo ao editor de cada plugin usado na chain. Formatos de plugins: arquitectura extensível; VST3 é o suporte auditado, não o limite do produto.
> Retirar Kick Design, Cymbal Design, instrumentos/efeitos próprios, D-Core/F-Core e restantes módulos antigos, samples antigos distribuídos e menus redundantes. Não apagar áudio do utilizador.
> [Direcção e plano de limpeza](docs/DIRECCAO-CORE-PATTERN-MAKER-TOOLS.md).

DAW desktop leve, fiável e fácil de usar, centrado em plugins externos e áudio importado/gravado. Windows 11 x64 é P0; uso privado, local-only, sem subscrição.

> **AI FORA DO CORE POR AGORA.**
> O Core é o DAW; o AICORE é o laboratório. Modelo, chaves, conhecimento, memória de receitas/SQLite, chats e JSONL pertencem ao futuro pacote externo, nunca ao Core.exe.
> **A bridge AICORE não pertence ao binário principal.** O Core conserva catálogo musical restrito, validação tudo-ou-nada e Undo.
> Mudanças de dependência, processo, persistência ou âmbito exigem decisão numerada do dono antes de código.

**Leitura obrigatória para trabalho nesta fronteira: [docs/FRONTEIRA-CORE-AICORE.md](docs/FRONTEIRA-CORE-AICORE.md).**

**Estado honesto:** no snapshot `599d558`, a bridge por ficheiros e o painel ainda estão ligados ao build. Não foram encontrados a opção `CORE_BUILD_AI` nem o registo de `ai_boundary_static` na configuração examinada. São lacunas a resolver, não isolamento já provado. Esta entrega muda apenas documentação.

- [Estado e relatórios](ESTADO.md)
- [Fronteira: finalidades, ficheiros, travessias, migração e testes](docs/FRONTEIRA-CORE-AICORE.md)
- [Auditoria de 17-11-2026](docs/relatorios/2026-11-17-auditoria-core.md)
- [Utilização no Windows](app/README.md)

O som criativo vem dos plugins. As tools Core orquestram plugins e dados musicais. O motor e o workflow principal têm de funcionar sem pacote AI instalado; essa independência exige teste, não uma promessa.
