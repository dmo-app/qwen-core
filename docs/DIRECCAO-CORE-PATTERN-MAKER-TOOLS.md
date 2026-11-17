# Direcção do Core — Pattern Maker + Tools

> **DIRECÇÃO EXPLÍCITA DO DONO — 17-11-2026**
> **Da antiga camada criativa ficam apenas Pattern Maker e Tools.**
> O som vem de plugins de terceiros instalados pelo utilizador e do áudio importado/gravado.
> Preservar o catálogo de padrões e o alinhamento de pratos com kick/baixo.
> Retirar instrumentos/efeitos próprios, os antigos módulos “*-Core”, os samples antigos distribuídos e a complexidade dos menus.

Este documento regista a clarificação do dono nesta conversa. É documentação e plano; não declara limpeza implementada. Não ratifica em bloco a folha D58–D66 sugerida pelo coordenador. Os IDs **PROD-01…PROD-07** abaixo servem para rastrear esta instrução explícita, sem ocupar ou renumerar decisões D do ledger.

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

Só ficam **Pattern Maker e Tools** como ferramentas criativas próprias do Core. Isto não remove o DAW: transporte, arrangement, mixer, hosting VST3, automação, routing, buses, gravação, import/export, persistência e recuperação continuam necessários.

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

Não transformar a limpeza numa nova coleção de painéis. Não portar integralmente UI antiga ou PR antigo por inércia; comparar cada alteração com esta direcção e aproveitar apenas o que a serve. Não importar a proposta D58 de F-Core como superfície autónoma.

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

- Apenas Pattern Maker e Tools aparecem como ferramentas criativas próprias.
- Nenhum menu oferece um instrumento/efeito criativo interno como se produzisse som.
- Tools actuam nas pistas/partes e nos compassos indicados, com chain externa e parâmetros identificáveis.
- Pattern Maker usa pistas existentes e preserva o catálogo; não depende dos samples antigos.
- Alinhar pratos com kick/baixo continua disponível e editável.
- Alteração composta aplica-se integralmente ou é recusada; Undo restaura o estado pertinente.
- Falta de plugin é explícita; não é substituída silenciosamente por som próprio.
- Pacote novo não distribui os samples antigos; upgrade não apaga áudio do utilizador.
- Core continua a funcionar sem AI.
- Windows/áudio/export só são declarados validados depois de execução registada.

## 5. Pontos técnicos ainda a fechar

A direcção de produto acima está decidida pelo pedido do dono. Não voltar a perguntar se D-Core/F-Core devem continuar como módulos.

Antes de implementar, falta concretizar:
- o inventário exacto de samples/recursos legados a retirar do pacote;
- quais receitas antigas sobrevivem em Tools e o mapeamento dos seus parâmetros para plugins externos;
- a semântica dos limites temporais: curvas e caudas após o fim, restauração de parâmetros e várias tools sobre a mesma instância;
- o tratamento de projectos antigos sem o plugin substituto instalado.

Recomendação: preservar primeiro padrões/alinhamento e integridade dos projectos, consolidar a interface, depois retirar recursos e código morto. Separar claramente estas decisões técnicas da direcção já expressa.
