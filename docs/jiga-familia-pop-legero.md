# Jiga: família Pop/Legero (Agility Legero V2.3 a V2.6)

Origem: `docs/TesteProgManutencao/`. Lista dos comandos que a jiga envia ao produto e do resultado esperado, para o port da função da jiga para o prog_wifi. Formato dos frames em `docs/jiga-protocol.md`.

Legenda: **[código]** vem do código ativo. **[comentário]** vem de comentário do código. **[inferido]** dedução, precisa de confirmação.

## Resumo

A jiga liga o motor, confere se o encoder mudou, desliga, aciona o sentido oposto, e repete até completar 3 ciclos. No fim envia um comando de configuração e aprova se a resposta trouxer `'O' 'K'`. [código]

## Identificação

- O produto anuncia o nome `AGILITY LEGERO  V2.3` (também V2.4, V2.5 e V2.6). Os dois espaços entre as palavras fazem parte do nome. [código]
- Se nenhum nome for reconhecido e o produto não responder às consultas `'C'` e `'L'`, a jiga trata o produto como Pop e executa o mesmo teste. [código]
- Antes de testar, a jiga espera 1000U para o produto iniciar, com o transmissor desligado. O comentário do código diz "5 segundos". [código] + [comentário]

## Frame

Pedido: `7E 08 4A ('J') <CMD1> <CMD2> <CMD3> <CKS> 81`

- `CMD2` vale 1 em todos os pedidos do Legero e `CMD3` vale 0. [código]
- Valores de `CMD1` usados, pelo significado dos comentários do código **[inferido]**:

| CMD1 | Significado |
|---|---|
| 11 | repouso |
| 1 | liga o motor |
| 2 | desliga o motor |
| 0 | aciona o sentido oposto |
| 7 | fim de curso |
| 6 | configuração |

## Esperas

`U` é a unidade de espera da jiga. O tempo real de 1U não foi medido. Pelo cálculo do código, 1U equivale a cerca de 15 voltas do laço principal. [código] + [inferido]

## Comandos de um ciclo

| # | Comando | Frame (CMD2 = 1) | Espera depois | Resultado esperado |
|---|---|---|---|---|
| 1 | Configuração (6) | `7E 08 4A ('J') 06 01 00 C4 81` | 100U | Resposta com ID `'J'`. Guarda o encoder B (byte `[9]` da resposta) |
| 2 | Repouso (11) | `7E 08 4A ('J') 0B 01 00 C9 81` | 0 | |
| 3 | Liga o motor (1) | `7E 08 4A ('J') 01 01 00 C3 81` | 1000U | Lê o encoder A (byte `[9]`) |
| 4 | Mantém o motor ligado (1) | `7E 08 4A ('J') 01 01 00 C3 81` | 1000U | Compara A com B. Diferentes: segue. Iguais: reprova |
| 5 | Desliga (2), duas vezes | `7E 08 4A ('J') 02 01 00 C0 81` | 10U cada | |
| 6 | Repouso (11) | `7E 08 4A ('J') 0B 01 00 C9 81` | 10U | |
| 7 | Sentido oposto (0), duas vezes | `7E 08 4A ('J') 00 01 00 C2 81` | 1000U cada | Nenhuma conferência |
| 8 | Repouso (11) | `7E 08 4A ('J') 0B 01 00 C9 81` | 10U | |
| 9 | Desliga (2) | `7E 08 4A ('J') 02 01 00 C0 81` | 10U | |
| 10 | Repouso (11) | `7E 08 4A ('J') 0B 01 00 C9 81` | 0 | Conta 1 ciclo. Menos de 3: volta ao comando 1 |

Duração de um ciclo: cerca de 4150U (100 + 1000 + 1000 + 10 + 10 + 10 + 1000 + 1000 + 10 + 10). [código] + [inferido]

## Comandos finais (depois de 3 ciclos)

| # | Comando | Frame (CMD2 = 1) | Espera depois | Resultado esperado |
|---|---|---|---|---|
| 11 | Fim de curso (7) | `7E 08 4A ('J') 07 01 00 C5 81` | 50U | |
| 12 | Configuração (6) | `7E 08 4A ('J') 06 01 00 C4 81` | 100U | Resposta com ID `'J'` |
| 13 | Leitura da resposta | | | Bytes `[4]` e `[5]` da resposta iguais a `'O'` e `'K'`: aprovado |

## Resultado

- Aprovado: o LCD mostra `    APROVADO    ` / `****************`, a jiga emite um beep e desliga o transmissor. [código]
- Reprovado: o LCD mostra `   REPROVADO    ` / `****************`, a jiga emite um beep e desliga o transmissor. Só acontece quando A é igual a B (comando 4). [código]
- Depois do resultado a jiga fica parada. Nada reinicia a detecção. [código]
- Durante o teste, a linha 2 do LCD mostra `ENC:eeeee C:ccc` (leitura do encoder e ciclos contados). [código]

## Falhas

| Sintoma | Significado |
|---|---|
| `REPROVADO` | O encoder não mudou depois de 1000U com o motor ligado. Motor parado, encoder sem pulso, ou nenhum frame novo chegou. [código] + [inferido] |
| Parado sem resultado nos comandos 1 ou 12 | Não chega resposta com ID `'J'`. Não há limite de tempo nem mensagem. [código] |
| Parado sem resultado no comando 13 | A resposta não traz `'O' 'K'`. Não há reprovação nem limite de tempo. [código] |
| Aprovado com o motor sem mover na volta | A volta não é verificada. [código] + [inferido] |
| Detecção repetindo | Nenhum nome foi reconhecido e as consultas `'C'` e `'L'` ficaram sem resposta. [código] |

## Pontos de melhoria e dúvidas

1. Só a ida é verificada, e só pela comparação de dois valores do encoder. Um encoder que mude uma vez e depois pare passa. [código] + [inferido]
2. O encoder tem 1 byte. Uma contagem que avance exatamente 256 posições dá leitura igual por acaso. É improvável. [inferido]
3. A jiga manda a configuração (6) antes de cada ciclo. Não se sabe o efeito disso no produto. [código]
4. A espera pela confirmação `'O' 'K'` não tem limite de tempo. [código]
5. As esperas são medidas em voltas do laço, então o tempo real do motor ligado varia com a carga do laço (LCD, UART). [código] + [comentário]
6. O valor de `CMD2` não é zerado entre produtos: depois de um Legero, uma Híbrida herda `CMD2 = 1`. [código]
7. O código não define o significado dos valores de `CMD1` e `CMD2`.
