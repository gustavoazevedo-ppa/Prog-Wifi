# Jiga: família Facility (Triflex Facility e Facility Axis)

Origem: `docs/TesteProgManutencao/`. Documento de estudo para o port da função da jiga para dentro do projeto prog_wifi (firmware ESP32, em C) e para achar pontos de melhoria no teste. Frames em `docs/jiga-protocol.md`, estados em `docs/jiga-state-machine.md`. Os dois documentos continuam valendo, e este repete o que for preciso para ser lido sozinho.

Legenda: **[código]** executado pelo código ativo. **[comentário]** vem de comentário do código. **[inferido]** dedução minha, precisa de confirmação.

## Resumo

Triflex Facility, conjunto de comandos 20, pedido com ID `'L'`. Mesmo fluxo da Full Range (estados 125 a 132, 3 ciclos de ida e volta controlados por posição do encoder). O Facility Axis (Triflex IA) segue exatamente o mesmo fluxo. A única diferença visível ao código é a resposta de 19 bytes em vez de 17. [código]

## Unidade de espera

`U` = 1 decremento de `TempoMaquina`. Ele cai uma vez a cada 200 chamadas de `UserIRQHandler` (2 x 10 x 10, constantes de `equates.h`), e o laço principal chama essa rotina 13 vezes por volta. Logo 1U ≈ 15,4 voltas do laço principal, sem contar as chamadas extras da ISR do TIMER_0 (1 a cada 50 ms). O tempo real de uma volta não foi estimado. [código] + [inferido]

| Espera | Voltas do laço (aprox.) |
|---|---|
| 30U | 462 |
| 254U | 3.908 |
| 300U (janela de probe) | 4.615 |

## 1. Identificação

| Critério | Detalhe | Proc. |
|---|---|---|
| Banner (caminho principal) | O nome contém `"Facility"`, por exemplo `STriflex Facility Axis`. É o **primeiro** critério testado. Resultado: letra `'L'`, conjunto 20 | [código] + [comentário] |
| Probe (fallback) | A jiga pede primeiro com `'C'`. Só se nenhuma resposta (ID 0 ou `'C'`) chegar na janela de 300U (≈ 4.615 voltas), pede com `'L'`. Resposta com ID `'L'` (`StartMaquina = 10`) fecha como Facility, conjunto 20, também com janela de 300U | [código] |
| Risco do probe | O comentário diz que o Facility também responde ao `'C'`. Se responder, o estado 201 fecha a detecção como Full Range ou Connect (conjunto 10) antes do probe `'L'`. Pelo banner isso não acontece, porque `"Facility"` é testado primeiro | [comentário] vs [código] |
| `ModeloCod` | 1 (`MODELO_TRIFLEX`) pelo probe, ou 4 pela tabela `"Triflex Facility      V2.1"`. Só afetam o LCD. A decisão do teste sai pelo banner ou pelo ID | [código] |
| Facility Axis | Mesmo caminho do Facility. Resposta de 19 bytes (tamanho `0x13`) | [código] + [comentário] |
| Contradição | Os comentários citam ID `'P'` para o Axis, mas a recepção Triflex só aceita ID `'L'`, `'C'` ou 0. Se o Axis responder `'P'`, o frame é descartado | [comentário] vs [código] |

Depois de identificar, a jiga espera 30U (≈ 462 voltas) e, no caminho por banner, mais 2 s reais antes de ligar o motor. No caminho por banner há uma tela temporária "RECONHECIDO" que bloqueia o laço por 4 s. [código]

## 2. Acionar

Pedido: `7E 08 4C 54 <CmdJiga> 20 <CKS> 51` (ID `'L'`). Bits de `CmdJiga`: `CMD_FWR` 0x04, `CMD_REV` 0x08, `CMD_HIGHsp` 0x10. [código]

Durante a detecção e nos intervalos o `CmdJiga` vale 0: `7E 08 4C 54 00 20 B1 51`.

| Estado | CmdJiga depois da ação | Frame enviado | Espera depois |
|---|---|---|---|
| 125 | `CMD_FWR` = 0x04 | `7E 08 4C 54 04 20 B5 51` | 254U |
| 126 | `CMD_FWR` + `CMD_REV` = 0x0C | `7E 08 4C 54 0C 20 BD 51` | 254U |
| 127 | 0x00 (zera `CMD_FWR` e `CMD_REV`) | `7E 08 4C 54 00 20 B1 51` | 254U |
| 128 | `CMD_HIGHsp` = 0x10 | `7E 08 4C 54 10 20 A1 51` | 254U |
| 129 | `CMD_HIGHsp` + `CMD_REV` = 0x18 | `7E 08 4C 54 18 20 A9 51` | 254U |
| 130 | 0x00 (zera `CMD_HIGHsp` e `CMD_REV`), `SomaCiclos++` | `7E 08 4C 54 00 20 B1 51` | 254U |
| 131 | 0x00 | `7E 08 4C 54 00 20 B1 51` | 254U (se < 3 ciclos) |

[código]. O estado 126 liga `CMD_REV` **sem** desligar `CMD_FWR`, então os dois bits ficam em 1 até o 127. Os dois conjuntos são espelhos um do outro: trocam `CMD_REV` e `CMD_HIGHsp`. [código] + [comentário]

## 3. Parar

Por posição, nas mesmas janelas das outras Triflex (`PlsEncoder` de 16 bits). [código]

| Estado | Janela lado A | Janela lado B |
|---|---|---|
| 126 partida | 50 a 150 | 65450 a 65500 |
| 127 parada (ida) | 700 a 900 | 64650 a 64800 |
| 129 volta | 500 a 600 | 64900 a 65000 |
| 130 fim | 10 a 100 | 65400 a 65550 |

Ciclos: 3 (`SomaCiclos >= 3` no estado 131), sem reinício da contagem. [código]

## 4. Voltar

A volta é o estado 128 (`CMD_HIGHsp` isolado, depois de 254U). No 129 liga `CMD_REV` junto, e o 130 desliga perto do ponto de partida. O estado 128 liga só `CMD_HIGHsp`, e o vigia de encoder não conta isso como motor comandado (só `CMD_FWR` ou `CMD_REV` contam). Por isso não há proteção contra encoder parado no 128. [código] + [inferido]

## 5. Fim do teste

Aprovado: ` Central Triflex ` / `  Aprovada !!!!` (`Frases_LCD[86]`) e beep no estado 131. O estado 132 deixa a jiga ociosa. [código]

## 6. O que o operador vê

Linha 2 do LCD: `ENC:eeeee C:ccc` (encoder e ciclos). Para o Facility pelo probe, a tela de identificação mostra `Triflex Facility      V2.1`. [código]

## 7. Anomalias

| Sintoma | O que significa | Proc. |
|---|---|---|
| `ERRO ENCODER` / `MOTOR SEM PULSO` | Motor comandado (`CMD_FWR` ou `CMD_REV`) e encoder sem mudar por 5 s reais, nos estados 125 a 130 | [código] |
| Detecção repete e o Axis não é encontrado | O Axis pode estar respondendo com ID `'P'`, que a recepção rejeita | [comentário] + [inferido] |
| Teste parado com o motor girando | Encoder passou pela janela entre leituras, ou nunca entrou nela. Não há limite de tempo nem erro | [código] + [inferido] |
| Aprovado depois de 1 ciclo | `SomaCiclos` com valor de execução anterior | [código] + [inferido] |
| Reportar | O texto do LCD, a linha `ENC:... C:...` e o estado em que parou | [inferido] |

## 8. Pontos de melhoria e dúvidas

1. No estado 130 o conjunto 20 grava `LadoMotorA`/`LadoMotorB` de forma assimétrica (no lado A grava B, no lado B grava B). Provável cópia e cola. `LadoMotorA` e `LadoMotorB` nunca são zerados. [código] + [inferido]
2. A tela temporária de 4 s e os demais pontos do Full Range também valem aqui. Ver `docs/jiga-familia-full-range.md`, seção 8.
3. O ID `'P'` do Axis nos comentários precisa ser conferido com a equipe do produto.
4. O estado 128 deixa o motor comandado só com `CMD_HIGHsp`, sem que isso seja visto como motor em movimento. [código] + [inferido]
