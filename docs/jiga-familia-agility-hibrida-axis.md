# Jiga: família Agility Híbrida e Agility Axis

Origem: `docs/TesteProgManutencao/`. Documento de estudo para o port da função da jiga para dentro do projeto prog_wifi (firmware ESP32, em C) e para achar pontos de melhoria no teste. Frames em `docs/jiga-protocol.md`, estados em `docs/jiga-state-machine.md`. Os dois documentos continuam valendo, e este repete o que for preciso para ser lido sozinho.

Legenda: **[código]** executado pelo código ativo. **[comentário]** vem de comentário do código. **[inferido]** dedução minha, precisa de confirmação.

## Resumo

Protocolo Pop (ID `'J'`), o mesmo fluxo do Legero (`docs/jiga-familia-pop-legero.md`). A jiga liga o motor por tempo fixo, lê o encoder (1 byte) duas vezes e reprova se ele não mudou, desliga, aciona o sentido oposto por tempo, e repete até contar 3 ciclos. No fim envia a configuração e aprova se a resposta trouxer `'O' 'K'`. A diferença para o Legero é que `CMD2` **não é alterado**: vale o que estava na jiga (0 ao ligar). [código]

Cobre a Agility Híbrida V1.4 a V1.7 e a Agility Axis V1.0 e V1.1. A Agility Axis V1.2 é reconhecida, mas não tem teste. Não confundir com o Triflex Facility Axis, que usa o protocolo Triflex. [código]

## Unidade de espera

`U` = 1 decremento de `TempoMaquina`. Ele cai uma vez a cada 200 chamadas de `UserIRQHandler` (2 x 10 x 10, constantes de `equates.h`), e o laço principal chama essa rotina 13 vezes por volta. Logo 1U ≈ 15,4 voltas do laço principal, sem contar as chamadas extras da ISR do TIMER_0 (1 a cada 50 ms). O tempo real de uma volta não foi estimado. [código] + [inferido]

| Espera | Voltas do laço (aprox.) |
|---|---|
| 10U | 154 |
| 20U | 308 |
| 50U | 769 |
| 100U | 1.538 |
| 1000U | 15.385 |

## 1. Identificação

| Critério | Detalhe | Proc. |
|---|---|---|
| Banner | O nome casa na tabela `ModeloTabela`: `"AGILITY HIBRIDA V1.4"` a `V1.7` (`ModeloCod` 7, 8, 9, 10), `"AGILITY  AXIS   V1.0"` e `V1.1` (`ModeloCod` 20 e 21). Os espaços fazem parte da busca | [código] |
| Decisão | No estado 207, depois de testar as palavras Triflex, qualquer `ModeloCod != 0` vai para o estado 202, sem usar os probes `'C'` e `'L'` | [código] |
| Via probe | Se não houver banner reconhecido, os probes ficam sem resposta e a jiga cai no estado 202 com `ChaveSerial = 2` | [código] |
| Espera antes de testar | O estado 202 espera 1000U (≈ 15.385 voltas) para o produto inicializar. O comentário diz "5 segundos". O TX Pop fica desligado nesse tempo | [código] + [comentário] |
| Axis V1.2 (`ModeloCod` 22) | Reconhecida pela tabela, mas o comparador do 202 lista V1.1 duas vezes e omite V1.2. Sem teste: a detecção recomeça | [código] |

## 2. Acionar

Pedido: `7E 08 4A <CMD1> <CMD2> <CMD3> <CKS> 81`, com `CMD3` sempre 0. Aqui `CMD2` vale 0 (valor inicial). [código]

Significado de `CMD1`, **[inferido]** de comentários curtos: 11 = repouso, 1 = liga motor, 2 = desliga, 0 = comando de sentido oposto, 7 = fim de curso, 6 = configuração.

| Passo | Estado | Entra quando | CMD1 | Frame enviado (CMD2 = 0) | Espera depois |
|---|---|---|---|---|---|
| Despacho | 202 (`TESTERot`) | Espera de 1000U zerada | 6 | `7E 08 4A 06 00 00 C5 81` | 10U |
| Resposta | 22 | Espera zerada e resposta com ID `'J'`. Lê o encoder B (`rx[9]`) | 6 (mantém) | `7E 08 4A 06 00 00 C5 81` | 100U |
| Repouso | 23 | Em toda passagem põe `CMD1 = 11`. Com a espera zerada passa a `CMD1 = 1` | 11, depois 1 | `7E 08 4A 0B 00 00 C8 81`, depois `7E 08 4A 01 00 00 C2 81` | 1000U |
| Lê encoder A | 24 | Espera zerada. Lê `rx[9]` | 1 | `7E 08 4A 01 00 00 C2 81` | 1000U |
| Confere | 25 | Espera zerada. Se B diferente de A: `CMD1 = 2`. Senão `CMD1 = 11` e vai ao 202 | 2, ou 11 | `7E 08 4A 02 00 00 C1 81` | 10U (ou 20U) |
| Desliga | 26 | Espera zerada | 2 | `7E 08 4A 02 00 00 C1 81` | 10U |
| Repouso | 27 | Espera zerada | 11 | `7E 08 4A 0B 00 00 C8 81` | 10U |
| Sentido oposto | 28 | Espera zerada | 0 | `7E 08 4A 00 00 00 C3 81` | 1000U |
| Sentido oposto | 29 | Espera zerada | 0 | `7E 08 4A 00 00 00 C3 81` | 1000U |
| Repouso | 30 | Espera zerada | 11 | `7E 08 4A 0B 00 00 C8 81` | 10U |
| Desliga | 31 | Espera zerada | 2 | `7E 08 4A 02 00 00 C1 81` | 10U |
| Conta ciclo | 32 | Espera zerada. `SomaCiclos++`. Menos de 3: `CMD1 = 11` e volta ao 22 | 11 | `7E 08 4A 0B 00 00 C8 81` | 0 |
| Fim de curso | 32 | Com `SomaCiclos >= 3`: `CMD1 = 7` | 7 | `7E 08 4A 07 00 00 C4 81` | 50U |
| Configuração | 33 | Espera zerada e resposta com ID `'J'`: `CMD1 = 6`, `CMD2` inalterado | 6 | `7E 08 4A 06 00 00 C5 81` | 100U |
| Confirma | 34 | Espera zerada e `rx[4] == 'O'` e `rx[5] == 'K'` | (TX para) | | |

## 3. Parar

Por **tempo**: o motor fica ligado (`CMD1 = 1`) por 2000U (≈ 30.769 voltas), do fim do estado 23 até o estado 25. A verificação é a comparação do encoder B (lido no 22, antes de ligar) com o A (lido no 24, 1000U depois de ligar). Diferentes: o motor se mexeu. Iguais: reprova. [código]

O encoder do Pop tem 1 byte (`rx[9]`). Não há janela de posição. Ciclos: 3 (`SomaCiclos >= 3` no estado 32). [código]

## 4. Voltar

O sentido oposto é `CMD1 = 0`, ligado nos estados 28 e 29 por 2000U (≈ 30.769 voltas). Os estados 28 e 29 também leem o encoder, mas nada compara a leitura da volta. O retorno não é verificado. [código] + [inferido]

Ciclo completo: ≈ 4150U (≈ 63.846 voltas). Três ciclos: ≈ 12.450U (≈ 191.538 voltas), mais a espera inicial de 1000U e o final de 50U + 100U. [código] + [inferido]

## 5. Fim do teste

- Aprovado (estado 34): `    APROVADO    ` / `****************` (`Frases_LCD[9]`), beep, e o TX Pop é desligado. [código]
- Reprovado (estado 202): `   REPROVADO    ` / `****************` (`Frases_LCD[10]`), beep, TX desligado. Só acontece se o encoder não mudou no estado 25. [código]
- A jiga fica ociosa. Nada reinicia a detecção. [código]

## 6. O que o operador vê

Linha 2 do LCD com `StartEncoder = 0x99`: `ENC:eeeee C:ccc` (o `PlsEncoder`, que no Pop recebe `rx[9]`, e os ciclos). [código]

## 7. Anomalias

| Sintoma | O que significa | Proc. |
|---|---|---|
| `REPROVADO` | O encoder lido antes de ligar o motor e 1000U depois é igual. Causas possíveis: motor não gira, encoder sem pulso, ou nenhum frame novo chegou | [código] + [inferido] |
| Parado sem resultado nos estados 22 ou 33 | Não chega resposta com ID `'J'`. Não há limite de tempo nem mensagem | [código] |
| Parado depois da configuração (estado 34) | Resposta sem `'O' 'K'`. Não há reprovação nesse caso | [código] |
| Detecção repetindo com Axis V1.2 | A família é reconhecida, mas o comparador não a inclui | [código] |
| Aprovado com o motor sem mover na volta | A volta não é verificada | [código] + [inferido] |
| Reportar | O texto do LCD, a linha `ENC:... C:...` e o estado em que parou | [inferido] |

## 8. Pontos de melhoria e dúvidas

1. Os pontos do Legero valem aqui: só a ida é verificada e só nos primeiros 1000U, leitura de encoder de 1 byte, `CMD1 = 6` desde o despacho, estado 34 sem reprovação, tempo medido em voltas do laço. Ver `docs/jiga-familia-pop-legero.md`, seção 8.
2. A Híbrida herda o `CMD2` do teste anterior, porque nada o zera. Depois de um Legero fica em 1. [código]
3. O comparador do estado 202 lista `AXIS_V1_1` duas vezes e omite `AXIS_V1_2`. Pode ser erro de digitação. [código] + [inferido]
4. Não se sabe se a Híbrida e a Axis deveriam receber `CMD2` diferente de 0 na configuração. O código só trata o Legero. [código]
