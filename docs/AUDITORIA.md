# Procedimento de auditoria

1. Ler AGENTS.md, ESTADO.md, produto e fronteira. Fixar branch/SHA, plataforma e âmbito.
2. Revalidar cada pendência no código actual. Não tratar o relatório antigo como descrição do HEAD.
3. Classificar achados: confirmado no código, comprovado por teste, apenas reportado, não verificado. Citar ficheiro:linha e SHA.
4. Separar bug de decisão de produto/arquitectura. Não implementar numa tarefa apenas de auditoria.
5. Quando houver implementação autorizada: testar regressão no anterior e no novo estado; registar comandos, códigos de saída, suites/casos/verificações, ambiente e limitações. Comparar contagens no mesmo âmbito.
6. Validar o que a mudança exige; um teste de domínio não comprova UI, Linux não comprova Windows. Não inventar tiers nem anunciar CI inexistente.
7. Entregar docs/relatorios/AAAA-MM-DD-assunto.md usando MODELO.md. Se houver dois relatórios do mesmo assunto/dia, usar sufixo descritivo.
8. Acrescentar entrada em docs/relatorios/README.md e actualizar ESTADO.md: o que fechou, o que continua, quem/qual decisão desbloqueia.
9. Relatório anterior mantém-se; correcção factual é addendum datado, não reescrita silenciosa. Evitar cópias integrais e “resumo do resumo”.

Concluir com próximos passos concretos. Se bloqueado por plataforma, dado ou decisão, deixar a prova disponível e a pergunta exacta; não marcar como corrigido/verificado.
