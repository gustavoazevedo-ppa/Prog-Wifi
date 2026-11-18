# Jiga: família Full Range

Origem: `docs/TesteProgManutencao/`. Documento de estudo para o port da função da jiga para dentro do projeto prog_wifi (firmware ESP32, em C) e para achar pontos de melhoria no teste. Frames em `docs/jiga-protocol.md`, estados em `docs/jiga-state-machine.md`. Os dois documentos continuam valendo, e este repete o que for preciso para ser lido sozinho.

Legenda: **[código]** executado pelo código ativo. **[comentário]** vem de comentário do código. **[inferido]** dedução minha, precisa de confirmação.

## Resumo

Triflex Full Range, conjunto de comandos 10. A jiga liga o motor para frente, para na posição lida no encoder, liga o motor no sentido oposto, para perto do ponto de partida, e repete até contar 3 ciclos. As mudanças de estado dependem da posição do encoder (16 bits) e de esperas. A única reprovação é `ERRO ENCODER`. [código]

## Unidade de espera

`U` = 1 decremento de `TempoMaquina`. Ele cai uma vez a cada 200 chamadas de `UserIRQHandler` (2 x 10 x 10, constantes de `equates.h`), e o laço principal chama essa rotina 13 vezes por volta. Logo 1U ≈ 15,4 voltas do laço principal, sem contar as chamadas extras da ISR do TIMER_0 (1 a cada 50 ms). O tempo real de uma volta não foi estimado: depende do LCD e de transmissões bloqueantes da UART. [código] + [inferido]

| Espera | Voltas do laço (aprox.) |
|---|---|
| 30U | 462 |
| 254U | 3.908 |
| 300U (janela de probe) | 4.615 |

## 1. Identificação

| Critério | Detalhe | Proc. |
|---|---|---|
| Banner (caminho principal) | O nome contém `"FULL"`, por exemplo `TRIFLEX FULL RANGE V3.14`. É testado depois de `"Facility"` e `"CONNECT"`. Resultado: letra `'C'`, conjunto 10 | [código] + [comentário] |
| Escuta do banner | Até 4 s reais (`ESCUTA_BANNER_US`). Termina antes se o nome chega (`ModeloRawIdx >= 6`) | [código] |
| Probe (fallback) | A jiga pede com ID `'C'`. Resposta com ID `0` (`StartMaquina = 5`) fecha no estado 201 como Full Range, conjunto 10. Janela de 300U (≈ 4.615 voltas) | [código] |
| Distinção do Connect | O Full Range responde ID `0`, o Connect responde ID `'C'` [comentário]. No estado 201 os dois caem no mesmo ramo (`StartMaquina == 5`) e recebem o mesmo conjunto 10. Pelo probe, o código ativo não os separa | [comentário] + [código] |
| `ModeloCod` | Fica 0. Não há entrada na tabela, e o nome aparece cru no LCD | [código] |

Depois de identificar, a jiga espera 30U (≈ 462 voltas) e pelo menos 2 s reais desde a identificação antes de ligar o motor (`produtoPronto`, estado 125). No caminho por banner há ainda uma tela temporária "RECONHECIDO" que bloqueia o laço por 4 s (`delay_real_ms(4000)`), então a espera de 2 s já terminou quando o estado 125 roda. [código]

## 2. Acionar

Pedido: `7E 08 43 54 <CmdJiga> 20 <CKS> 51`. Bits de `CmdJiga`: `CMD_FWR` 0x04, `CMD_REV` 0x08, `CMD_HIGHsp` 0x10. [código]

Durante a detecção e nos intervalos o `CmdJiga` vale 0: `7E 08 43 54 00 20 BE 51`.

| Estado | Entra quando | CmdJiga depois da ação | Frame enviado | Espera depois |
|---|---|---|---|---|
| 125 | Primeira vez: espera da detecção zerada e produto pronto. Nos ciclos seguintes `ApenasCicloM == 22` basta, sem espera | `CMD_FWR` = 0x04 | `7E 08 43 54 04 20 BA 51` | 254U |
| 126 | Espera zerada e encoder na janela de partida (tabela abaixo) | `CMD_FWR` + `CMD_HIGHsp` = 0x14 | `7E 08 43 54 14 20 AA 51` | 254U |
| 127 | Espera zerada e encoder na janela de parada | 0x00 (zera `CMD_FWR` e `CMD_HIGHsp`) | `7E 08 43 54 00 20 BE 51` | 254U |
| 128 | Espera zerada | `CMD_REV` = 0x08 | `7E 08 43 54 08 20 B6 51` | 254U |
| 129 | Espera zerada e encoder na janela de volta | `CMD_REV` + `CMD_HIGHsp` = 0x18 | `7E 08 43 54 18 20 A6 51` | 254U |
| 130 | Espera zerada e encoder na janela de fim | `CMD_HIGHsp` e `CMD_FWR` a 0, `CMD_REV` continua: 0x08. `SomaCiclos++` | `7E 08 43 54 08 20 B6 51` | 254U |
| 131 | Espera zerada | 0x00 | `7E 08 43 54 00 20 BE 51` | 254U (se < 3 ciclos) |

[código]. O significado de "alta velocidade" para `CMD_HIGHsp` vem do comentário do código ("seletor de velocidade, não comando de giro").

## 3. Parar

A parada na ida é por **posição**: o estado 127 só desliga o motor quando `PlsEncoder` entra na janela. Comparações estritas (`>` e `<`). Se o encoder nunca entra na janela, o estado espera para sempre e o motor continua comandado. [código]

`PlsEncoder` = byte [5] x 256 + byte [6] da resposta. O lado A conta a partir de ~0 para cima. O lado B conta a partir de 65535 para baixo (virada do contador). [código] + [inferido]

| Estado | Janela lado A | Janela lado B |
|---|---|---|
| 126 partida | 50 a 150 | 65450 a 65500 |
| 127 parada (ida) | 700 a 900 | 64650 a 64800 |
| 129 volta | 500 a 600 | 64900 a 65000 |
| 130 fim | 10 a 100 | 65400 a 65550 |

No 127 e no 129 o lado precisa ter sido marcado antes (`LadoMotorA` ou `LadoMotorB` = 0x33). A distância de ida é então de ~700 a ~900 contagens a partir da posição inicial. [código] + [inferido]

**Ciclos:** a jiga conta 3 ciclos completos (`SomaCiclos >= 3` no estado 131). A contagem nunca é zerada, então uma segunda execução sem reiniciar a jiga aprova após o primeiro ciclo. [código]

## 4. Voltar

A volta é o estado 128: liga `CMD_REV` (sentido oposto) depois da espera de 254U. Nos estados 129 e 130 a jiga acelera (`CMD_HIGHsp`) e desliga perto do ponto de partida pela mesma regra de janelas. [código]

## 5. Fim do teste

- Aprovado: o estado 131 zera os bits de comando e mostra ` Central Triflex ` / `  Aprovada !!!!` (`Frases_LCD[86]`) com beep. O estado 132 deixa a jiga ociosa. O TX Triflex continua pedindo. [código]
- Nada reinicia a detecção depois do fim. Para testar outro produto, a jiga precisa ser reiniciada. [código] + [inferido]

## 6. O que o operador vê

Linha 2 do LCD durante o teste: `ENC:eeeee C:ccc`, ou seja, o `PlsEncoder` e o número de ciclos contados. [código]

## 7. Anomalias

| Sintoma | O que significa | Proc. |
|---|---|---|
| `ERRO ENCODER` / `MOTOR SEM PULSO` e beep | O motor foi comandado (`CMD_FWR` ou `CMD_REV`) e o `PlsEncoder` não mudou por 5 s reais, nos estados 125 a 130. Causas possíveis: motor parado, encoder sem pulso, frames de resposta deixando de chegar (o valor só atualiza com frame válido) | [código] + [inferido] |
| Motor girando, encoder mudando, mas o teste não avança | O encoder passou pela janela entre duas leituras, ou nunca entrou nela. Não há limite de tempo nem erro nesse caso | [código] + [inferido] |
| Nome do modelo parado no LCD e teste não começa | O estado 125 não ligou o motor: espera da detecção, `produtoPronto` ou conjunto de comandos ainda sem valor | [código] + [inferido] |
| Detecção repetindo (nome do produto não aparece) | Nenhuma família respondeu ao banner nem aos probes | [código] |
| Aprovado depois de 1 ciclo | `SomaCiclos` já vinha com valor de uma execução anterior | [código] + [inferido] |
| Reportar | O texto do LCD, a linha `ENC:... C:...` e o estado em que parou (125 a 132) | [inferido] |

## 8. Pontos de melhoria e dúvidas

1. A tela temporária de 4 s ("MEDIÇÃO TEMPORÁRIA - REMOVER DEPOIS") bloqueia o laço e faz a espera real de 2 s do estado 125 não ter efeito no caminho por banner. [código] + [comentário]
2. Falta um limite de tempo por passo (126 a 130). Hoje só o vigia do encoder, que não pega o caso de encoder em movimento fora das janelas. [código] + [inferido]
3. As janelas são estreitas (50 a 200 contagens) e dependem da leitura cair dentro delas. [código]
4. No estado 130 o conjunto 10 grava `LadoMotorA` no caminho do lado B (provável cópia e cola), e `LadoMotorA`/`LadoMotorB` nunca são zerados. Depois do primeiro ciclo, as janelas dos dois lados ficam válidas. [código] + [inferido]
5. `SomaCiclos` e `ApenasCicloM` não são reiniciados entre execuções. [código]
6. O estado 130 deixa `CMD_REV` ligado até o 131. [código]
7. Não se sabe a escala do encoder (contagens por volta) nem a velocidade com que ele passa pelas janelas. Esperas em tempo real, e não em voltas do laço, tornariam o teste previsível.
