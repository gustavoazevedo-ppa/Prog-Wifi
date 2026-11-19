# Jiga: família Agility Híbrida e Agility Axis

Origem: `docs/TesteProgManutencao/`. Lista dos comandos que a jiga envia ao produto e do resultado esperado, para o port da função da jiga para o prog_wifi. Formato dos frames em `docs/jiga-protocol.md`.

Legenda: **[código]** vem do código ativo. **[comentário]** vem de comentário do código. **[inferido]** dedução, precisa de confirmação.

## Resumo

Mesmo teste do Legero (`docs/jiga-familia-pop-legero.md`). A jiga liga o motor, confere se o encoder mudou, desliga, aciona o sentido oposto, e repete até completar 3 ciclos. No fim envia a configuração e aprova se a resposta trouxer `'O' 'K'`. A diferença para o Legero é que `CMD2` **não é alterado**: vale o que estava na jiga (0 ao ligar). [código]

Cobre a Agility Híbrida V1.4 a V1.7 e a Agility Axis V1.0 e V1.1. A Agility Axis V1.2 é reconhecida, mas não tem teste. Não confundir com o Triflex Facility Axis, que usa o protocolo Triflex. [código]

## Identificação

- O produto anuncia o nome `AGILITY HIBRIDA V1.4` a `V1.7`, ou `AGILITY  AXIS   V1.0` e `V1.1`. Os espaços fazem parte do nome. [código]
- Se nenhum nome for reconhecido e o produto não responder às consultas `'C'` e `'L'`, a jiga executa o mesmo teste. [código]
- Antes de testar, a jiga espera 1000U para o produto iniciar, com o transmissor desligado. O comentário do código diz "5 segundos". [código] + [comentário]
- Agility Axis V1.2: o nome é reconhecido, mas a lista de produtos com teste lista a V1.1 duas vezes e omite a V1.2. Sem teste, a detecção recomeça. [código]

## Frame

Pedido: `7E 08 4A ('J') <CMD1> <CMD2> <CMD3> <CKS> 81`, com `CMD3` sempre 0. Aqui `CMD2` vale 0. [código]

Valores de `CMD1` usados, pelo significado dos comentários do código **[inferido]**:

| CMD1 | Significado |
|---|---|
| 11 | repouso |
| 1 | liga o motor |
| 2 | desliga o motor |
| 0 | aciona o sentido oposto |
| 7 | fim de curso |
| 6 | configuração |

`U` é a unidade de espera da jiga. O tempo real de 1U não foi medido (cerca de 15 voltas do laço principal). [código] + [inferido]

## Comandos de um ciclo

| # | Comando | Frame (CMD2 = 0) | Espera depois | Resultado esperado |
|---|---|---|---|---|
| 1 | Configuração (6) | `7E 08 4A ('J') 06 00 00 C5 81` | 100U | Resposta com ID `'J'`. Guarda o encoder B (byte `[9]` da resposta) |
| 2 | Repouso (11) | `7E 08 4A ('J') 0B 00 00 C8 81` | 0 | |
| 3 | Liga o motor (1) | `7E 08 4A ('J') 01 00 00 C2 81` | 1000U | Lê o encoder A (byte `[9]`) |
| 4 | Mantém o motor ligado (1) | `7E 08 4A ('J') 01 00 00 C2 81` | 1000U | Compara A com B. Diferentes: segue. Iguais: reprova |
| 5 | Desliga (2), duas vezes | `7E 08 4A ('J') 02 00 00 C1 81` | 10U cada | |
| 6 | Repouso (11) | `7E 08 4A ('J') 0B 00 00 C8 81` | 10U | |
| 7 | Sentido oposto (0), duas vezes | `7E 08 4A ('J') 00 00 00 C3 81` | 1000U cada | Nenhuma conferência |
| 8 | Repouso (11) | `7E 08 4A ('J') 0B 00 00 C8 81` | 10U | |
| 9 | Desliga (2) | `7E 08 4A ('J') 02 00 00 C1 81` | 10U | |
| 10 | Repouso (11) | `7E 08 4A ('J') 0B 00 00 C8 81` | 0 | Conta 1 ciclo. Menos de 3: volta ao comando 1 |

Duração de um ciclo: cerca de 4150U. [código] + [inferido]

## Comandos finais (depois de 3 ciclos)

| # | Comando | Frame (CMD2 = 0) | Espera depois | Resultado esperado |
|---|---|---|---|---|
| 11 | Fim de curso (7) | `7E 08 4A ('J') 07 00 00 C4 81` | 50U | |
| 12 | Configuração (6) | `7E 08 4A ('J') 06 00 00 C5 81` | 100U | Resposta com ID `'J'` |
| 13 | Leitura da resposta | | | Bytes `[4]` e `[5]` da resposta iguais a `'O'` e `'K'`: aprovado |

## Resultado

- Aprovado: o LCD mostra `    APROVADO    ` / `****************`, a jiga emite um beep e desliga o transmissor. [código]
- Reprovado: o LCD mostra `   REPROVADO    ` / `****************`, a jiga emite um beep e desliga o transmissor. Só acontece quando A é igual a B (comando 4). [código]
- Depois do resultado a jiga fica parada. Nada reinicia a detecção. [código]
- Durante o teste, a linha 2 do LCD mostra `ENC:eeeee C:ccc`. [código]

## Falhas

| Sintoma | Significado |
|---|---|
| `REPROVADO` | O encoder não mudou depois de 1000U com o motor ligado. Motor parado, encoder sem pulso, ou nenhum frame novo chegou. [código] + [inferido] |
| Parado sem resultado nos comandos 1 ou 12 | Não chega resposta com ID `'J'`. Não há limite de tempo nem mensagem. [código] |
| Parado sem resultado no comando 13 | A resposta não traz `'O' 'K'`. Não há reprovação. [código] |
| Detecção repetindo com a Axis V1.2 | O produto é reconhecido, mas não está na lista com teste. [código] |
| Aprovado com o motor sem mover na volta | A volta não é verificada. [código] + [inferido] |

Ao reportar, informar o texto do LCD e a linha `ENC:... C:...`. [inferido]

## Pontos de melhoria e dúvidas

1. Os pontos do Legero valem aqui: só a ida é verificada, leitura de encoder de 1 byte, configuração enviada antes de cada ciclo, comando 13 sem reprovação, tempo medido em voltas do laço. Ver `docs/jiga-familia-pop-legero.md`.
2. A Híbrida herda o `CMD2` do teste anterior, porque nada o zera. Depois de um Legero fica em 1. [código]
3. A lista de produtos com teste repete a Axis V1.1 e omite a V1.2. Pode ser erro de digitação. [código] + [inferido]
4. Não se sabe se a Híbrida e a Axis deveriam receber `CMD2` diferente de 0 na configuração. O código só trata o Legero. [código]
