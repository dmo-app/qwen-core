# Regras de trabalho — qwen-core

## Autoridade e âmbito

- Ler README.md, ESTADO.md e os documentos de produto/fronteira antes de trabalhar.
- A instrução actual do dono prevalece. Documento histórico, comentário de código, PR ou sugestão de coordenador não ratifica uma decisão.
- Produto: docs/DIRECCAO-CORE-PATTERN-MAKER-TOOLS.md. Processos/dependências/persistência/AI: docs/FRONTEIRA-CORE-AICORE.md.
- PROD-01…PROD-10 registam pedidos explícitos. P-F* são propostas. D58–D66 não foram ratificadas em bloco; não executar a folha do coordenador como autorização.
- Arquitectura e desenho final de novas superfícies exigem decisão explícita numerada. Não voltar a pedir decisões de produto já expressas; concretizar apenas os detalhes ainda abertos.
- Não reintroduzir instrumentos/designers próprios ou AI no Core. Não mover código entre Core/AICORE sem decisão.
- Preservar catálogo de padrões, alinhamento kick/baixo/pratos, projectos e áudio do utilizador.

## Execução

- Identificar branch/SHA e alterações existentes antes de editar; preservar trabalho alheio.
- Auditoria/documentação não autoriza implementação. Implementação autorizada deve ter âmbito explícito.
- Não portar PRs antigos integralmente: comparar com a baseline e trazer apenas o que falta e serve a direcção actual.
- Não apagar código, dados, testes ou história apenas por parecerem antigos; mapear dependências. Limpeza de samples distribuídos não é limpeza dos ficheiros do utilizador.
- Correcção de bug: regressão significativa e vermelho medido no estado anterior quando executável, depois verde. Se não executável, declarar a limitação.
- Contagens de testes por suite/âmbito; não reduzir cobertura ou relaxar critérios para obter verde. Flaky é defeito a investigar.
- Cada milestone de implementação mantém Core compilável, a abrir, tocar, guardar/reabrir e exportar; não declarar estes gates sem execução.
- Usar testes/gates registados no checkout. Linux não prova runtime Windows. Sem tokens novos para agentes ou activação de CI nesta organização documental.
- Não enviar mensagens, fechar PRs nem arquivar outro repo por inferência de um plano.

## Entrega obrigatória

Seguir docs/AUDITORIA.md. Deixar relatório em docs/relatorios/AAAA-MM-DD-assunto.md, indexá-lo e actualizar ESTADO.md com evidência e pendências.
Distinguir confirmado no código / comprovado por teste / apenas reportado / não verificado.
Nunca apresentar plano como implementado, teste existente como corrido, ou build como teste de UI.
