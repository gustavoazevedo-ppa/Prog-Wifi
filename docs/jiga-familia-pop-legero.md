# Jiga: família Pop/Legero (Agility Legero V2.3 a V2.6)

Origem: `docs/TesteProgManutencao/`. Documento de estudo para o port da função da jiga para dentro do projeto prog_wifi (firmware ESP32, em C) e para achar pontos de melhoria no teste. Frames em `docs/jiga-protocol.md`, estados em `docs/jiga-state-machine.md`. Os dois documentos continuam valendo, e este repete o que for preciso para ser lido sozinho.

Legenda: **[código]** executado pelo código ativo. **[comentário]** vem de comentário do código. **[inferido]** dedução minha, precisa de confirmação.

## Resumo

Protocolo Pop (ID `'J'`). A jiga liga o motor por tempo fixo, lê o encoder (1 byte) duas vezes e reprova se ele não mudou, desliga, aciona o sentido oposto também por tempo, e repete até contar 3 ciclos. No fim envia um comando de configuração e aprova se a resposta trouxer `'O' 'K'`. O Legero se distingue da Híbrida/Axis por mandar `CMD2 = 1` em todos os pedidos desde o início. [código]

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
| Banner | O nome casa na tabela `ModeloTabela` com `"AGILITY LEGERO  V2.3"` (V2.4, V2.5 e V2.6 também). Os espaços fazem parte da busca. `ModeloCod` = 2, 17, 18 ou 19 | [código] |
| Decisão | No estado 207, depois de testar as palavras Triflex (`"Facility"`, `"CONNECT"`, `"FULL"`), qualquer `ModeloCod != 0` vai direto para o estado 202, sem usar os probes `'C'` e `'L'` | [código] |
| Via probe | Se não houver banner reconhecido, os probes `'C'` e `'L'` ficam sem resposta e a jiga cai no estado 202 com `ChaveSerial = 2` | [código] |
| Espera antes de testar | O estado 202 espera 1000U (≈ 15.385 voltas) para o produto inicializar. O comentário diz "5 segundos". O TX Pop fica desligado nesse tempo | [código] + [comentário] |

## 2. Acionar

Pedido: `7E 08 4A <CMD1> <CMD2> <CMD3> <CKS> 81`, com `CMD3` sempre 0. No Legero `CMD2` vale 1 em todos os pedidos. [código]

Significado de `CMD1`, **[inferido]** de comentários curtos ("liga motor", "desliga motor", "fim de curso"): 11 = repouso, 1 = liga motor, 2 = desliga, 0 = comando de sentido oposto, 7 = fim de curso, 6 = configuração.

| Passo | Estado | Entra quando | CMD1 | Frame enviado (CMD2 = 1) | Espera depois |
|---|---|---|---|---|---|
| Despacho | 202 (`TESTERot`) | Espera de 1000U zerada | 6 | `7E 08 4A 06 01 00 C4 81` | 10U |
| Resposta | 22 | Espera zerada e resposta com ID `'J'`. Lê o encoder B (`rx[9]`) | 6 (mantém) | `7E 08 4A 06 01 00 C4 81` | 100U |
| Repouso | 23 | Em toda passagem põe `CMD1 = 11`. Com a espera zerada passa a `CMD1 = 1` | 11, depois 1 | `7E 08 4A 0B 01 00 C9 81`, depois `7E 08 4A 01 01 00 C3 81` | 1000U |
| Lê encoder A | 24 | Espera zerada. Lê `rx[9]` | 1 | `7E 08 4A 01 01 00 C3 81` | 1000U |
| Confere | 25 | Espera zerada. Se B diferente de A: `CMD1 = 2`. Senão `CMD1 = 11` e vai ao 202 | 2, ou 11 | `7E 08 4A 02 01 00 C0 81` | 10U (ou 20U) |
| Desliga | 26 | Espera zerada | 2 | `7E 08 4A 02 01 00 C0 81` | 10U |
| Repouso | 27 | Espera zerada | 11 | `7E 08 4A 0B 01 00 C9 81` | 10U |
| Sentido oposto | 28 | Espera zerada | 0 | `7E 08 4A 00 01 00 C2 81` | 1000U |
| Sentido oposto | 29 | Espera zerada | 0 | `7E 08 4A 00 01 00 C2 81` | 1000U |
| Repouso | 30 | Espera zerada | 11 | `7E 08 4A 0B 01 00 C9 81` | 10U |
| Desliga | 31 | Espera zerada | 2 | `7E 08 4A 02 01 00 C0 81` | 10U |
| Conta ciclo | 32 | Espera zerada. `SomaCiclos++`. Menos de 3: `CMD1 = 11` e volta ao 22 | 11 | `7E 08 4A 0B 01 00 C9 81` | 0 |
| Fim de curso | 32 | Com `SomaCiclos >= 3`: `CMD1 = 7` | 7 | `7E 08 4A 07 01 00 C5 81` | 50U |
| Configuração | 33 | Espera zerada e resposta com ID `'J'`: `CMD1 = 6`, `CMD2 = 1` | 6 | `7E 08 4A 06 01 00 C4 81` | 100U |
| Confirma | 34 | Espera zerada e `rx[4] == 'O'` e `rx[5] == 'K'` | (TX para) | | |

## 3. Parar

Por **tempo**: o motor fica ligado (`CMD1 = 1`) por 2000U (≈ 30.769 voltas), do fim do estado 23 até o estado 25. A única verificação é a comparação do encoder B (lido no estado 22, antes de ligar) com o A (lido no estado 24, 1000U depois de ligar). Diferentes: o motor se mexeu. Iguais: reprova. [código]

O encoder do Pop tem 1 byte (`rx[9]`). Não há janela de posição. Ciclos: 3 (`SomaCiclos >= 3` no estado 32). [código]

## 4. Voltar

O sentido oposto é `CMD1 = 0`, ligado nos estados 28 e 29 por 2000U (≈ 30.769 voltas), com `CMD2 = 1`. Os estados 28 e 29 também leem o encoder, mas nada compara a leitura da volta. O retorno não é verificado. [código] + [inferido]

Ciclo completo: ≈ 4150U (≈ 63.846 voltas): 100 + 1000 + 1000 + 10 + 10 + 10 + 1000 + 1000 + 10 + 10. Três ciclos: ≈ 12.450U (≈ 191.538 voltas), mais a espera inicial de 1000U e o final de 50U + 100U. [código] + [inferido]

## 5. Fim do teste

- Aprovado (estado 34): `    APROVADO    ` / `****************` (`Frases_LCD[9]`), beep, e o TX Pop é desligado. [código]
- Reprovado (estado 202): `   REPROVADO    ` / `****************` (`Frases_LCD[10]`), beep, TX desligado. Só acontece se o encoder não mudou no estado 25. [código]
- A jiga fica ociosa. Nada reinicia a detecção. [código]

## 6. O que o operador vê

Linha 2 do LCD com `StartEncoder = 0x99`: `ENC:eeeee C:ccc` (o `PlsEncoder`, que no Pop recebe `rx[9]`, e os ciclos contados). [código]

## 7. Anomalias

| Sintoma | O que significa | Proc. |
|---|---|---|
| `REPROVADO` | O encoder lido antes de ligar o motor e 1000U depois é igual. Causas possíveis: motor não gira, encoder sem pulso, ou nenhum frame novo chegou | [código] + [inferido] |
| Parado sem resultado nos estados 22 ou 33 | Não chega resposta com ID `'J'`. Não há limite de tempo nem mensagem | [código] |
| Parado depois da configuração (estado 34) | Resposta sem `'O' 'K'`. Não há reprovação nesse caso | [código] |
| Aprovado com o motor sem mover na volta | A volta não é verificada | [código] + [inferido] |
| Aprovado depois de 1 ciclo | `SomaCiclos` com valor de execução anterior | [código] + [inferido] |
| Detecção repetindo | Nenhuma entrada da tabela casou e os probes não tiveram resposta | [código] |
| Reportar | O texto do LCD, a linha `ENC:... C:...` e o estado em que parou (22 a 34 ou 202) | [inferido] |

## 8. Pontos de melhoria e dúvidas

1. Só a ida é verificada, e só nos primeiros 1000U. Um encoder que mude uma vez e depois pare passa. [código] + [inferido]
2. Um byte de encoder pode dar leitura igual por acaso se a contagem avançar exatamente 256 posições. É improvável, mas possível. [inferido]
3. A jiga manda `CMD1 = 6` (configuração) desde o despacho até o estado 23, antes do teste. Não se sabe o efeito disso no produto. [código]
4. O estado 34 não tem reprovação nem tempo limite: uma configuração recusada deixa a jiga parada. [código]
5. O tempo é medido em voltas do laço, então o motor pode ficar ligado por mais ou menos tempo conforme o que o laço estiver fazendo (LCD, UART). [código] + [comentário]
6. `CMD2` não é zerado entre produtos: depois de um Legero, uma Híbrida herda `CMD2 = 1`. [código]
7. Os significados dos valores de `CMD1` e `CMD2` não estão definidos no código.
