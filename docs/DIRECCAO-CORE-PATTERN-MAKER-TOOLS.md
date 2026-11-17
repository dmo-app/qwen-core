# Direcção do Core — Pattern Maker + Tools

> **DIRECÇÃO EXPLÍCITA DO DONO — 17-11-2026**
> **Da antiga camada criativa ficam apenas Pattern Maker e Tools.**
> O som vem de plugins de terceiros instalados pelo utilizador e do áudio importado/gravado.
> Preservar o catálogo de padrões e o alinhamento de pratos com kick/baixo.
> Retirar instrumentos/efeitos próprios, os antigos módulos “*-Core”, os samples antigos distribuídos e a complexidade dos menus.

Este documento regista a clarificação do dono nesta conversa. É documentação e plano; não declara limpeza implementada. Não ratifica em bloco a folha D58–D66 sugerida pelo coordenador. Os IDs **PROD-01…PROD-10** abaixo servem para rastrear esta instrução explícita, sem ocupar ou renumerar decisões D do ledger.

A fronteira de processos, memória e AI continua definida em [FRONTEIRA-CORE-AICORE.md](FRONTEIRA-CORE-AICORE.md). AI permanece diferida; Pattern Maker e Tools têm de funcionar manualmente sem modelo, pacote AI ou AICORE.

## 1. O que fica

### PROD-01 — Tools

Tools são funções de criação musical que configuram e agrupam plugins externos numa chain e controlam os parâmetros relevantes para criar um momento/efeito musical.

O utilizador define:

- as pistas ou partes das pistas afectadas;
- um intervalo seleccionado, ou início/fim em compassos;
- os plugins da chain, a sua ordem e os parâmetros que a tool deve controlar.

A tool aplica a operação nesse contexto, com resultado editável e Undo. Build-up, transições, pump, rolls e efeitos semelhantes podem continuar como operações/receitas dentro de Tools quando compatíveis com o mandato. Não precisam de voltar como produtos, painéis ou “plugins Core” separados.

Exemplo ilustrativo, não funcionalidade já verificada: seleccionar duas pistas e os compassos 17–24; uma tool configura filtro/reverb/delay externos e as respectivas curvas para preparar o drop. Não cria um sintetizador, filtro ou reverb próprio do host.

### PROD-02 — Pattern Maker

Usa as pistas do projecto para criar/editar padrões. Trabalha com notas, eventos, clips e relações temporais; os instrumentos das pistas são plugins externos instalados. Não introduz um gerador de som próprio.

O percurso pattern → arrangement continua parte do workflow. O nome de produto visível é **Pattern Maker**; não manter P-Core como uma segunda ferramenta equivalente.

### PROD-03 — preservar catálogo e alinhamento

Manter:

- o catálogo de padrões existente;
- as variações e receitas rítmicas úteis, incluindo KBBB/rolls;
- a capacidade de alinhar pratos com baixo e kick;
- edição posterior e Undo das alterações.

A preservação do catálogo foi agora pedida explicitamente. Os 45 JSON inventariados na fronteira permanecem dados musicais do Core, com a proveniência e o status hypothesis existentes. A origem AICORE não obriga a instalar AICORE nem autoriza apagar padrões.

Preservar o comportamento musical útil não exige conservar nomes, classes ou painéis antigos. Ao implementar, localizar e testar os percursos actuais de alinhamento antes de refactorizar; não afirmar nesta entrega que todas as variantes já foram verificadas em runtime.

## 2. O que sai

### PROD-04 — instrumentos, efeitos próprios e módulos antigos

Retirar da oferta do produto os instrumentos e efeitos criativos próprios e todos os módulos “*-Core” como superfícies autónomas, incluindo **D-Core e F-Core**, e os restantes equivalentes.

Só ficam **Pattern Maker e Tools** como ferramentas criativas próprias do Core. Isto não remove o DAW: transporte, arrangement, mixer, hosting de plugins externos (VST3 no estado auditado; extensível a outros formatos), automação, routing, buses, gravação, import/export, persistência e recuperação continuam necessários.

Gain, pan, metering, resampling e protecção de saída permanecem infraestrutura do host.

A semântica útil de uma função antiga pode ser absorvida por Tools, usando plugins de terceiros. Não apagar automaticamente um algoritmo de padrões/alinhamento por estar num ficheiro com um nome antigo. Também não manter DSP criativo próprio com um nome novo.

### PROD-05 — samples antigos distribuídos

Retirar os samples antigos fornecidos pelo Core do pacote, menus, sugestões por omissão, presets/demo e mecanismos de cópia automática que os distribuam.

**Não significa apagar samples pessoais, áudio gravado/importado ou ficheiros referenciados por projectos do utilizador.** A leitura de projectos existentes deve continuar a identificar essas referências e permitir localização/substituição explícita.

Inventariar antes de remover: recursos realmente distribuídos, assets dos templates/demo e ficheiros copiados pelo instalador. O catálogo de padrões não é uma sample library e não deve ser removido em conjunto. Presets de notas devem sobreviver sem dependência dos samples antigos.

Se um projecto antigo dependia de instrumento próprio, preservar os dados e indicar o estado não suportado/silencioso, com substituição explícita por plugin externo; não inventar equivalência sonora. Se dependia de sample antigo ainda presente no computador, não o apagar silenciosamente.

## 3. PROD-06 — limpeza extensa dos menus

Objectivo: uma operação deve ter um lugar primário claro, poucos passos e profundidade acessível quando necessária.

| Área | Direcção |
| --- | --- |
| Ferramentas criativas | Só Pattern Maker e Tools; operações antigas úteis tornam-se acções/receitas destas |
| Instrumentos/FX | Apenas plugins externos instalados como escolhas criativas; sem “Core's own instruments” |
| Selecção de alvo | Pistas/partes seleccionadas ou intervalo início/fim em compassos; alvo sempre explícito |
| Menus de pista/clip | Acções pertinentes ao objecto; eliminar entradas obsoletas, duplicadas e sem efeito |
| Routing/buses | Preservar função de DAW e agrupamento por papéis quando útil; sem exigir abrir D-Core/R-Core |
| Parâmetros avançados | Dentro do contexto da chain/tool, acessíveis sob pedido |
| Samples antigos | Não oferecer nem reinstalar conteúdo descontinuado |
| Estado indisponível | Distinguir plugin em falta, acção incompatível e legado silencioso; sem opção aparentemente activa que não faz nada |
| AI | Não obrigatória nem reactivada por esta reorganização |

Os painéis de Tools são agora um âmbito explícito de exploração pedido pelo dono (PROD-08). Devem simplificar tarefas e manter-se dentro de Tools, sem reconstruir a antiga coleção de módulos autónomos. O desenho final ainda não está decidido. Não portar integralmente UI antiga ou PR antigo por inércia; comparar cada alteração com esta direcção e aproveitar apenas o que a serve. Não importar a proposta D58 de F-Core como superfície autónoma.

## 4. PROD-07 — execução posterior com preservação

A tarefa actual é documental. Plano para a implementação posterior:

1. **Inventário:** mapear cards, menus, atalhos, módulos, DSP criativo, samples distribuídos, templates e dependências de projectos; localizar catálogo e alinhamento que têm de sobreviver.
2. **Provas de preservação:** estabelecer casos para Pattern Maker, catálogo completo, alinhamento kick/baixo/pratos, selecção de pistas e intervalos, e undo.
3. **Tools:** reaproveitar orquestração determinística e ligar plugins externos, separando-a do DSP próprio e dos nomes antigos. Instância/ordem/estado/automação são responsabilidades do host.
4. **Menus:** consolidar os percursos em Pattern Maker/Tools, remover escolhas de instrumentos próprios e acções antigas sem função.
5. **Pacote:** retirar samples antigos e referências de distribuição; adaptar templates/demo sem destruir conteúdo dos projectos existentes.
6. **Limpeza interna:** só retirar código antigo depois de mapear dependências e provar que não leva consigo padrões/alinhamento ou compatibilidade de leitura.
7. **Qualificação Windows:** compilar, abrir, tocar, guardar, reabrir e exportar em cada milestone; verificar ausência do pacote AI.

Cada conjunto deve deixar um Core utilizável. Não substituir testes de regressão do produto por simples contagens de ficheiros removidos.

### Critérios de aceitação

- Apenas Pattern Maker e Tools aparecem como ferramentas criativas próprias; Kick Design e Cymbal Design não são oferecidos.
- O painel de Tools permite abrir directamente o editor de cada instância usada na chain, sem a recriar.
- Os painéis simplificam o percurso sem impedir acesso aos parâmetros completos dos plugins.
- Formatos adicionais têm estado de suporte e testes explícitos; VST3 não é um limite permanente do produto.
- Nenhum menu oferece um instrumento/efeito criativo interno como se produzisse som.
- Tools actuam nas pistas/partes e nos compassos indicados, com chain externa e parâmetros identificáveis.
- Pattern Maker usa pistas existentes e preserva o catálogo; não depende dos samples antigos.
- Alinhar pratos com kick/baixo continua disponível e editável.
- Alteração composta aplica-se integralmente ou é recusada; Undo restaura o estado pertinente.
- Falta de plugin é explícita; não é substituída silenciosamente por som próprio.
- Pacote novo não distribui os samples antigos; upgrade não apaga áudio do utilizador.
- Core continua a funcionar sem AI.
- Windows/áudio/export só são declarados validados depois de execução registada.

## 5. Clarificações adicionais do dono

### PROD-08 — explorar painéis simplificados de Tools

**A forma final de Tools ainda está por explorar.** A descrição funcional anterior não fecha o desenho da interface nem exige que o utilizador construa manualmente toda a chain em cada utilização.

Investigar painéis que simplifiquem o percurso: escolher o momento/efeito pretendido, indicar pistas/partes e intervalo em compassos, usar uma chain de plugins externos e ajustar os parâmetros relevantes. Mostrar primeiro poucos controlos claros; permitir aprofundar parâmetros, ordem e routing quando necessário. Presets/receitas e mapeamentos são possibilidades a estudar, não uma interface já implementada ou aprovada.

**Acesso directo aos plugins, por pedido explícito do dono:** o painel deve mostrar os plugins usados na chain e permitir abrir o editor de cada um a partir da própria entrada. Tem de abrir a instância exacta que está nessa chain, não outra instância do mesmo plugin nem uma cópia nova. Fechar e reabrir o editor não deve recriar o plugin ou repor os parâmetros.

Os controlos simplificados não substituem o editor completo. A relação entre macros da Tool, parâmetros do plugin e automação deve ser visível e coerente; definir como são tratadas alterações manuais em parâmetros automatizados, sem valores divergentes ou sobrescritas silenciosas. Se o plugin estiver ausente ou não disponibilizar editor, comunicar a situação e o percurso de parâmetros disponível, sem oferecer um botão que parece funcionar e não faz nada.

Estudo posterior esperado:
- comparar esboços de painéis sobre tarefas reais, não apenas estética;
- explicar o que cada painel controla e quais plugins/instâncias altera;
- demonstrar selecção de pistas e início/fim em compassos;
- demonstrar abertura directa de cada editor e retorno ao painel sem perder contexto;
- comparar passos/cliques e quantidade de controlos visíveis;
- preservar acesso à profundidade e ao Undo;
- apresentar o desenho ao dono antes de o implementar.

Este estudo fica registado para desenvolvimento e explicação futuros. Não há mockup final nem painel novo implementado nesta entrega.

### PROD-09 — retirar Kick Design e Cymbal Design

**Kick Design e Cymbal Design entram explicitamente na remoção dos instrumentos/designers próprios.** A criação sonora de kicks e pratos é feita nos plugins externos que o utilizador instala.

Retirar essas ofertas dos painéis, menus, atalhos e conteúdo distribuído que as apresente como instrumentos/designers do Core; inventariar dependências antes de remover código. Não basta mudar-lhes o nome para Tools e conservar síntese/DSP criativo próprio.

Isto não remove a criação de padrões de kick/pratos, o alinhamento de pratos com kick/baixo, nem a possibilidade de abrir e configurar plugins externos a partir de Tools. São funções diferentes: organizar eventos musicais fica no Core; gerar o timbre criativo pertence ao plugin.

O pedido confirma a intenção de remoção; a presença e os caminhos exactos de todos os designers no snapshot actual ainda precisam do inventário de implementação. Não declarar remoção concluída nesta tarefa documental.

### PROD-10 — plugins externos sem limitar o produto a VST3

**A direcção do produto é adaptar-se a diferentes tipos e formatos de plugins externos, não ser exclusivamente VST3.** A referência a VST3 descreve o suporte identificado na auditoria, não um limite permanente da finalidade do Core.

Tools e Pattern Maker devem trabalhar com as capacidades necessárias — instrumentos, efeitos, parâmetros, estado, automação, entradas/saídas e editor quando disponível — sem tornar o workflow musical dependente do nome de um formato. O hosting deve poder ser ampliado através de adaptações por formato, preservando identidade de instância, estado, timing e recuperação.

“Todo tipo de plugin” é o objectivo de abrangência, **não uma promessa de compatibilidade universal já entregue**. Cada formato e plugin precisa de verificação das capacidades, plataforma, requisitos de distribuição/licenciamento e testes; Windows 11 x64 continua P0. Não afirmar que um formato é utilizável numa plataforma sem o verificar.

Antes de implementar suporte adicional, preparar uma matriz com: formato/plataforma, estado de suporte, scan, carregamento, editor, parâmetros/automação, estado/save/reopen, routing/latência e recuperação. Identificar claramente suportado, parcial, planeado e não verificado. Nenhum novo SDK/backend foi adicionado nesta entrega.

## 6. Pontos técnicos ainda a fechar

A direcção de produto acima está decidida pelo pedido do dono. Não voltar a perguntar se D-Core/F-Core devem continuar como módulos.

Antes de implementar, falta concretizar:

- o desenho dos painéis simplificados de Tools e a relação macro/automação/editor de cada instância;
- a matriz e prioridade de formatos de plugins a suportar para além de VST3;
- o inventário exacto de Kick Design/Cymbal Design e samples/recursos legados a retirar do pacote;
- quais receitas antigas sobrevivem em Tools e o mapeamento dos seus parâmetros para plugins externos;
- a semântica dos limites temporais: curvas e caudas após o fim, restauração de parâmetros e várias tools sobre a mesma instância;
- o tratamento de projectos antigos sem o plugin substituto instalado.

Recomendação: preservar primeiro padrões/alinhamento e integridade dos projectos, consolidar a interface, depois retirar recursos e código morto. Separar claramente estas decisões técnicas da direcção já expressa.
