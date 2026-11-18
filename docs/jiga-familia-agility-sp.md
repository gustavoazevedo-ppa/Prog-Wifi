# Jiga: família Agility SP (V1.0 a V1.3)

Origem: `docs/TesteProgManutencao/`. Documento de estudo para o port da função da jiga para dentro do projeto prog_wifi (firmware ESP32, em C) e para achar pontos de melhoria no teste. Frames em `docs/jiga-protocol.md`, estados em `docs/jiga-state-machine.md`. Os dois documentos continuam valendo, e este repete o que for preciso para ser lido sozinho.

Legenda: **[código]** executado pelo código ativo. **[comentário]** vem de comentário do código. **[inferido]** dedução minha, precisa de confirmação.

## Resumo

Protocolo Pop (ID `'J'`), sem verificação de movimento. A jiga liga o motor, desliga, aciona o sentido oposto e desliga, tudo por tempo fixo, 3 vezes. No fim envia a configuração e aprova se a resposta trouxer `'O' 'K'`. Não lê o encoder para decidir nada e **não tem caminho de reprovação**. Quem percebe que o motor não girou é o operador. [código]

Um comentário do código diz que o SP pode ser um modelo sem encoder, onde contagem parada é informação sobre o produto e não defeito. É por isso que o vigia de encoder não roda nesse fluxo. Não se sabe se o SP tem encoder. [comentário]

## Unidade de espera

`U` = 1 decremento de `TempoMaquina`. Ele cai uma vez a cada 200 chamadas de `UserIRQHandler` (2 x 10 x 10, constantes de `equates.h`), e o laço principal chama essa rotina 13 vezes por volta. Logo 1U ≈ 15,4 voltas do laço principal, sem contar as chamadas extras da ISR do TIMER_0 (1 a cada 50 ms). O tempo real de uma volta não foi estimado. [código] + [inferido]

| Espera | Voltas do laço (aprox.) |
|---|---|
| 10U | 154 |
| 50U | 769 |
| 100U | 1.538 |
| 1000U | 15.385 |

## 1. Identificação

| Critério | Detalhe | Proc. |
|---|---|---|
| Banner | O nome casa na tabela `ModeloTabela` com `"AGILITY SP      V1.0"` (V1.1, V1.2 e V1.3 também). Os espaços fazem parte da busca. `ModeloCod` = 6, 14, 15 ou 16 | [código] |
| Decisão | No estado 207, depois de testar as palavras Triflex, qualquer `ModeloCod != 0` vai para o estado 202, sem usar os probes `'C'` e `'L'` | [código] |
| Via probe | Se não houver banner reconhecido, os probes ficam sem resposta e a jiga cai no estado 202 com `ChaveSerial = 2` | [código] |
| Espera antes de testar | O estado 202 espera 1000U (≈ 15.385 voltas) para o produto inicializar. O comentário diz "5 segundos". O TX Pop fica desligado nesse tempo | [código] + [comentário] |
| Despacho | `CtrlManutencao = 50`, `ChavaModeloPop = 5`, TX Pop ligado, espera de 10U. **Não** põe `CMD1 = 6` (a Híbrida e o Legero põem) | [código] |

## 2. Acionar

Pedido: `7E 08 4A <CMD1> <CMD2> <CMD3> <CKS> 81`, com `CMD3` sempre 0. `CMD2` vale 0 (valor inicial). `CMD1` começa em 11 (valor inicial). [código]

Significado de `CMD1`, **[inferido]** de comentários curtos: 11 = repouso, 1 = liga motor, 2 = desliga, 0 = comando de sentido oposto, 7 = fim de curso, 6 = configuração.

| Estado | Entra quando | CMD1 depois da ação | Frame enviado | Espera depois |
|---|---|---|---|---|
| (despacho) | Espera de 1000U zerada | 11 (inicial) | `7E 08 4A 0B 00 00 C8 81` | 10U |
| 50 | Espera zerada e resposta com ID `'J'` | 11 | `7E 08 4A 0B 00 00 C8 81` | 100U |
| 51 | Espera zerada | 1 (liga motor) | `7E 08 4A 01 00 00 C2 81` | 1000U |
| 52 | Espera zerada | 1 | `7E 08 4A 01 00 00 C2 81` | 1000U |
| 53 | Espera zerada | 2 (desliga) | `7E 08 4A 02 00 00 C1 81` | 1000U |
| 54 | Espera zerada | 2 | `7E 08 4A 02 00 00 C1 81` | 1000U |
| 55 | Espera zerada | 11 | `7E 08 4A 0B 00 00 C8 81` | 1000U |
| 56 | Espera zerada | 0 (sentido oposto) | `7E 08 4A 00 00 00 C3 81` | 1000U |
| 57 | Espera zerada | 0 | `7E 08 4A 00 00 00 C3 81` | 1000U |
| 58 | Espera zerada | 2 | `7E 08 4A 02 00 00 C1 81` | 1000U |
| 59 | Espera zerada | 2 | `7E 08 4A 02 00 00 C1 81` | 1000U |
| 60 | Espera zerada. `SomaCiclos++`. Menos de 3: `CMD1 = 11` e volta ao 50. 3 ou mais: `CMD1 = 7` | 11, ou 7 | `7E 08 4A 0B 00 00 C8 81`, ou `7E 08 4A 07 00 00 C4 81` | 1000U, ou 50U |
| 61 | Espera zerada e resposta com ID `'J'`: `CMD1 = 6` | 6 | `7E 08 4A 06 00 00 C5 81` | 100U |
| 62 | Espera zerada e `rx[4] == 'O'` e `rx[5] == 'K'` | (TX para) | | |

## 3. Parar

Só por **tempo**, sem leitura de encoder:

| Fase | CMD1 | Duração |
|---|---|---|
| Ligado, ida | 1 | 2000U (≈ 30.769 voltas), do estado 51 ao 53 |
| Desligado | 2 | 2000U, do 53 ao 55 |
| Repouso | 11 | 1000U, do 55 ao 56 |
| Ligado, sentido oposto | 0 | 2000U, do 56 ao 58 |
| Desligado | 2 | 2000U, do 58 ao 60 |
| Repouso | 11 | 1100U, do 60 até o 51 do ciclo seguinte |

Ciclo completo: ≈ 10.100U (≈ 155.385 voltas). Três ciclos: ≈ 30.300U (≈ 466.154 voltas), mais a espera inicial e o final de 50U + 100U. Ciclos: 3 (`SomaCiclos >= 3` no estado 60). [código] + [inferido]

## 4. Voltar

O sentido oposto é `CMD1 = 0`, ligado nos estados 56 e 57 (2000U), seguido de 2000U de `CMD1 = 2`. Nenhuma posição é verificada. [código]

## 5. Fim do teste

- Aprovado (estado 62): `    APROVADO    ` / `****************` (`Frases_LCD[9]`), beep, e o TX Pop é desligado. [código]
- Não existe reprovação: nenhum estado leva a `REPROVADO`. Sem resposta com ID `'J'` (50 e 61) ou sem `'O' 'K'` (62) a jiga espera para sempre. [código]
- A jiga fica ociosa. Nada reinicia a detecção. [código]

## 6. O que o operador vê

Linha 2 do LCD com `StartEncoder = 0x99`: `ENC:eeeee C:ccc` (o `PlsEncoder`, que no Pop recebe `rx[9]`, e os ciclos). Para o SP, é o único retorno visual de movimento, e a jiga não o usa para decidir. [código] + [inferido]

## 7. Anomalias

| Sintoma | O que significa | Proc. |
|---|---|---|
| Aprovado com o motor parado | A jiga não verifica movimento. O `APROVADO` só prova resposta `'J'` e `'O' 'K'` | [código] |
| Parado sem resultado nos estados 50 ou 61 | Não chega resposta com ID `'J'`. Não há limite de tempo nem mensagem | [código] |
| Parado depois da configuração (estado 62) | Resposta sem `'O' 'K'`. Não há reprovação | [código] |
| Linha `ENC:...` sem variar durante o movimento | Pode ser produto sem encoder ou encoder parado. A jiga não distingue | [inferido] |
| Detecção repetindo | Nenhuma entrada da tabela casou e os probes não tiveram resposta | [código] |
| Reportar | O texto do LCD, a linha `ENC:... C:...`, o estado em que parou (50 a 62) e o que o operador viu no motor | [inferido] |

## 8. Pontos de melhoria e dúvidas

1. O teste não verifica nada sobre movimento. Se o SP tiver encoder, a leitura de `rx[9]` já chega à jiga e poderia ser comparada como no Legero. Se não tiver, falta outro critério (corrente, fim de curso, confirmação do operador). [código] + [inferido]
2. Os tempos de desligado e repouso são 100 vezes maiores que no fluxo Pop com encoder (1000U contra 10U). Um ciclo do SP dura cerca de 2,4 vezes mais que o do Legero ou da Híbrida. Não se sabe se isso é necessário. [código] + [inferido]
3. Não há limite de tempo nem erro nos estados que aguardam resposta. [código]
4. `CMD2` não é zerado entre produtos. O SP herda o valor do teste anterior (por exemplo 1, depois de um Legero). [código]
5. O estado 52 e o 57 só esperam, sem mudar o `CMD1`. Podem ser etapas vazias. [código] + [inferido]
6. O tempo é medido em voltas do laço, e não em tempo real. [código] + [comentário]
7. Os significados dos valores de `CMD1` e `CMD2` não estão definidos no código.
