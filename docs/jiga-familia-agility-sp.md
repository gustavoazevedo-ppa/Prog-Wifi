# Jiga: família Agility SP (V1.0 a V1.3)

Origem: `docs/TesteProgManutencao/`. Lista dos comandos que a jiga envia ao produto e do resultado esperado, para o port da função da jiga para o prog_wifi. Formato dos frames em `docs/jiga-protocol.md`.

Legenda: **[código]** vem do código ativo. **[comentário]** vem de comentário do código. **[inferido]** dedução, precisa de confirmação.

## Resumo

A jiga liga o motor, desliga, aciona o sentido oposto e desliga, tudo por tempo fixo, 3 vezes. No fim envia a configuração e aprova se a resposta trouxer `'O' 'K'`. Não lê o encoder para decidir nada e **não tem caminho de reprovação**. Quem percebe que o motor não girou é o operador. [código]

Um comentário do código diz que o SP pode ser um modelo sem encoder, onde contagem parada é informação sobre o produto e não defeito. Por isso o vigia de encoder não roda neste teste. Não se sabe se o SP tem encoder. [comentário]

## Identificação

- O produto anuncia o nome `AGILITY SP      V1.0` (também V1.1, V1.2 e V1.3). Os espaços fazem parte do nome. [código]
- Se nenhum nome for reconhecido e o produto não responder às consultas `'C'` e `'L'`, a jiga executa o mesmo teste. [código]
- Antes de testar, a jiga espera 1000U para o produto iniciar, com o transmissor desligado. O comentário do código diz "5 segundos". [código] + [comentário]
- Diferente do Legero e da Híbrida, a jiga **não** envia a configuração (6) antes do teste. Envia o repouso (11) e espera 10U. [código]

## Frame

Pedido: `7E 08 4A ('J') <CMD1> <CMD2> <CMD3> <CKS> 81`, com `CMD3` sempre 0. `CMD2` vale 0 e `CMD1` começa em 11. [código]

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

Cada ciclo começa esperando uma resposta com ID `'J'`.

| # | Comando | Frame | Espera depois |
|---|---|---|---|
| 1 | Repouso (11) | `7E 08 4A ('J') 0B 00 00 C8 81` | 100U |
| 2 | Liga o motor (1) | `7E 08 4A ('J') 01 00 00 C2 81` | 1000U |
| 3 | Mantém o motor ligado (1) | `7E 08 4A ('J') 01 00 00 C2 81` | 1000U |
| 4 | Desliga (2) | `7E 08 4A ('J') 02 00 00 C1 81` | 1000U |
| 5 | Desliga (2) | `7E 08 4A ('J') 02 00 00 C1 81` | 1000U |
| 6 | Repouso (11) | `7E 08 4A ('J') 0B 00 00 C8 81` | 1000U |
| 7 | Sentido oposto (0) | `7E 08 4A ('J') 00 00 00 C3 81` | 1000U |
| 8 | Sentido oposto (0) | `7E 08 4A ('J') 00 00 00 C3 81` | 1000U |
| 9 | Desliga (2) | `7E 08 4A ('J') 02 00 00 C1 81` | 1000U |
| 10 | Desliga (2) | `7E 08 4A ('J') 02 00 00 C1 81` | 1000U |
| 11 | Repouso (11), conta 1 ciclo | `7E 08 4A ('J') 0B 00 00 C8 81` | 1000U. Menos de 3 ciclos: volta ao comando 1 |

Resultado esperado: nenhum. A jiga não confere movimento, encoder nem resposta nos comandos do ciclo, só a resposta com ID `'J'` no início. [código]

Duração de um ciclo: cerca de 10.100U. Três ciclos: cerca de 30.300U, mais a espera inicial e os comandos finais. [código] + [inferido]

## Comandos finais (depois de 3 ciclos)

| # | Comando | Frame | Espera depois | Resultado esperado |
|---|---|---|---|---|
| 12 | Fim de curso (7) | `7E 08 4A ('J') 07 00 00 C4 81` | 50U | |
| 13 | Configuração (6) | `7E 08 4A ('J') 06 00 00 C5 81` | 100U | Resposta com ID `'J'` |
| 14 | Leitura da resposta | | | Bytes `[4]` e `[5]` da resposta iguais a `'O'` e `'K'`: aprovado |

## Resultado

- Aprovado: o LCD mostra `    APROVADO    ` / `****************`, a jiga emite um beep e desliga o transmissor. [código]
- Não existe reprovação. Sem resposta com ID `'J'` (início de ciclo ou comando 13) ou sem `'O' 'K'` (comando 14), a jiga espera para sempre. [código]
- Depois do resultado a jiga fica parada. Nada reinicia a detecção. [código]
- Durante o teste, a linha 2 do LCD mostra `ENC:eeeee C:ccc` (leitura do encoder e ciclos contados). Para o SP é o único retorno visual de movimento, e a jiga não o usa. [código] + [inferido]

## Falhas

| Sintoma | Significado |
|---|---|
| Aprovado com o motor parado | A jiga não verifica movimento. O `APROVADO` só prova resposta `'J'` e `'O' 'K'`. [código] |
| Parado sem resultado no início de um ciclo ou no comando 13 | Não chega resposta com ID `'J'`. Não há limite de tempo nem mensagem. [código] |
| Parado depois da configuração (comando 14) | Resposta sem `'O' 'K'`. Não há reprovação. [código] |
| Linha `ENC:...` sem variar durante o movimento | Pode ser produto sem encoder ou encoder parado. A jiga não distingue. [inferido] |
| Detecção repetindo | Nenhum nome foi reconhecido e as consultas `'C'` e `'L'` ficaram sem resposta. [código] |

Ao reportar, informar o texto do LCD, a linha `ENC:... C:...` e o que o operador viu no motor. [inferido]

## Pontos de melhoria e dúvidas

1. O teste não verifica nada sobre movimento. Se o SP tiver encoder, a leitura do byte `[9]` já chega à jiga e poderia ser comparada como no Legero. Se não tiver, falta outro critério (corrente, fim de curso, confirmação do operador). [código] + [inferido]
2. As esperas de desligado e de repouso são 100 vezes maiores que no teste com encoder (1000U contra 10U). Um ciclo do SP dura cerca de 2,4 vezes mais que o do Legero ou da Híbrida. Não se sabe se isso é necessário. [código] + [inferido]
3. Não há limite de tempo nem erro nas esperas por resposta. [código]
4. `CMD2` não é zerado entre produtos. O SP herda o valor do teste anterior (por exemplo 1, depois de um Legero). [código]
5. Os comandos 3 e 8 só repetem o comando anterior, sem mudar nada. Podem ser etapas vazias. [código] + [inferido]
6. O tempo é medido em voltas do laço, e não em tempo real. [código] + [comentário]
7. Os significados dos valores de `CMD1` e `CMD2` não estão definidos no código.
