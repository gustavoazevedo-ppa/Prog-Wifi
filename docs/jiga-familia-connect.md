# Jiga: família Connect (Triflex Connect)

Origem: `docs/TesteProgManutencao/`. Documento de estudo para o port da função da jiga para dentro do projeto prog_wifi (firmware ESP32, em C) e para achar pontos de melhoria no teste. Frames em `docs/jiga-protocol.md`, estados em `docs/jiga-state-machine.md`. Os dois documentos continuam valendo, e este repete o que for preciso para ser lido sozinho.

Legenda: **[código]** executado pelo código ativo. **[comentário]** vem de comentário do código. **[inferido]** dedução minha, precisa de confirmação.

## Resumo

O Connect usa o mesmo fluxo de teste da Triflex (estados 125 a 132, 3 ciclos de ida e volta controlados por posição do encoder). O que muda é o **conjunto de comandos do motor**, que depende de como o Connect foi detectado: 20 pelo banner, 10 pelo probe. O código não diz qual dos dois realmente move o motor. [código] + [comentário]

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
| Banner (caminho principal) | O nome contém `"CONNECT"`, por exemplo `TRIFLEX CONNECT v2.34` (o nome chega cortado no fim). Testado depois de `"Facility"`. Resultado: letra `'C'`, conjunto `POLARIDADE_CONNECT` = 20 em `TESTERot.c` | [código] + [comentário] |
| Probe (fallback) | O Connect responde ao pedido `'C'` com ID `'C'` (`StartMaquina = 5`, `RespondeuIDC = 1`). O estado 201 testa `StartMaquina == 5` primeiro e fecha ali, com letra `'C'` e conjunto **10**, sem chegar ao probe `'L'`. Janela de 300U (≈ 4.615 voltas) | [código] |
| Probe `'L'` | Só ocorre se a resposta ao `'C'` não chegar na janela. Se o `'L'` também não responder e `RespondeuIDC` estiver ligado, o estado 205 volta para `'C'` com conjunto 10 | [código] |
| Distinção do Facility | O comentário diz que o Facility também responde ao probe `'C'` e que o `'L'` separaria os dois. Pelo código do estado 201, um Facility que responda `'C'` seria fechado como Full Range ou Connect antes do `'L'`. Pelo probe, Connect e Full Range também não se distinguem (ambos viram conjunto 10) | [comentário] vs [código] |
| Variantes pelo nome | `"3.15"`, `"2.34"`, `"2.36"` e `"2.45"` geram `ModeloCod` 23 a 26. Só mudam a tela do LCD (`TRIFLEX CONNECT TESTE CL`), não o teste | [código] |
| Firmware antigo sem banner | Se o buffer enche sem nome e há ID `'C'`, a jiga assume Connect pelo código 23 ou 24 | [código] |

Depois de identificar, a jiga espera 30U (≈ 462 voltas) e, no caminho por banner, mais 2 s reais antes de ligar o motor. No caminho por banner há uma tela temporária "RECONHECIDO" que bloqueia o laço por 4 s. [código]

## 2. Acionar

Pedido: `7E 08 43 54 <CmdJiga> 20 <CKS> 51`. Bits de `CmdJiga`: `CMD_FWR` 0x04, `CMD_REV` 0x08, `CMD_HIGHsp` 0x10. [código]

### Conjunto 20 (identificado pelo banner)

| Estado | CmdJiga depois da ação | Frame enviado | Espera depois |
|---|---|---|---|
| 125 | `CMD_FWR` = 0x04 | `7E 08 43 54 04 20 BA 51` | 254U |
| 126 | `CMD_FWR` + `CMD_REV` = 0x0C | `7E 08 43 54 0C 20 B2 51` | 254U |
| 127 | 0x00 (zera `CMD_FWR` e `CMD_REV`) | `7E 08 43 54 00 20 BE 51` | 254U |
| 128 | `CMD_HIGHsp` = 0x10 | `7E 08 43 54 10 20 AE 51` | 254U |
| 129 | `CMD_HIGHsp` + `CMD_REV` = 0x18 | `7E 08 43 54 18 20 A6 51` | 254U |
| 130 | 0x00 (zera `CMD_HIGHsp` e `CMD_REV`), `SomaCiclos++` | `7E 08 43 54 00 20 BE 51` | 254U |
| 131 | 0x00 | `7E 08 43 54 00 20 BE 51` | 254U (se < 3 ciclos) |

### Conjunto 10 (identificado pelo probe)

Igual ao conjunto 10 do Full Range (`docs/jiga-familia-full-range.md`, seção 2): 0x04, 0x14, 0x00, 0x08, 0x18, 0x08 (com `SomaCiclos++`), 0x00.

O comentário em `TESTERot.c` diz que o conjunto 10 não funciona para o Connect: ele responde ao probe e **para de responder** assim que `CMD_HIGHsp` vai a 1 (o byte [4] deixa de ser 0). O conjunto 20 é o outro aceito por um produto neste projeto. O resultado em bancada do 20 não está registrado. [comentário]

## 3. Parar

Por posição, nas mesmas janelas da Triflex (`PlsEncoder` de 16 bits). O código dos estados 126 a 130 é compartilhado com Full Range e Facility. [código]

| Estado | Janela lado A | Janela lado B |
|---|---|---|
| 126 partida | 50 a 150 | 65450 a 65500 |
| 127 parada (ida) | 700 a 900 | 64650 a 64800 |
| 129 volta | 500 a 600 | 64900 a 65000 |
| 130 fim | 10 a 100 | 65400 a 65550 |

Ciclos: 3 (`SomaCiclos >= 3` no estado 131), sem reinício da contagem. [código]

## 4. Voltar

No conjunto 20 a volta é o estado 128 (`CMD_HIGHsp`), seguido do 129 (`CMD_REV` com `CMD_HIGHsp`) e do 130 (para perto de 10 a 100). No conjunto 10 a volta usa `CMD_REV` no 128. [código]

## 5. Fim do teste

Aprovado: ` Central Triflex ` / `  Aprovada !!!!` (`Frases_LCD[86]`) e beep no estado 131. O estado 132 deixa a jiga ociosa. [código]

## 6. O que o operador vê

Linha 2 do LCD: `ENC:eeeee C:ccc` (encoder e ciclos). [código]

## 7. Anomalias

| Sintoma | O que significa | Proc. |
|---|---|---|
| `ERRO ENCODER` / `MOTOR SEM PULSO` | Motor comandado (`CMD_FWR` ou `CMD_REV`) e encoder sem mudar por 5 s reais, nos estados 125 a 130 | [código] |
| Connect para de responder logo que o motor liga | É o sintoma descrito no comentário para o conjunto 10 (`CMD_HIGHsp` em 1). Se aparecer com o conjunto 20, é um dado novo | [comentário] + [inferido] |
| Motor não gira, sem erro | Conjunto de comandos errado para o produto, ou o encoder não chega às janelas e o teste fica parado sem mensagem | [inferido] |
| Aprovado depois de 1 ciclo | `SomaCiclos` com valor de execução anterior | [código] + [inferido] |
| Reportar | Como foi detectado (banner ou probe), o texto do LCD, a linha `ENC:... C:...` e o estado em que parou | [inferido] |

## 8. Pontos de melhoria e dúvidas

1. O mesmo produto recebe comandos diferentes conforme o caminho de detecção. Convém decidir qual conjunto vale e eliminar a divergência (20 pelo banner, 10 pelo probe). [código]
2. `POLARIDADE_CONNECT` está definido duas vezes, com valores diferentes (20 em `TESTERot.c`, 10 em `ModoManutencao.c`). Só o de `TESTERot.c` é usado. [código]
3. Valem aqui os pontos do Full Range: tela temporária de 4 s, falta de limite de tempo por passo, janelas estreitas, `SomaCiclos` e `LadoMotorA`/`LadoMotorB` nunca zerados. Ver `docs/jiga-familia-full-range.md`, seção 8.
4. Se o Connect só responde enquanto `CMD_HIGHsp` é 0, os estados 126 e 129 com o conjunto 10 derrubam a comunicação. O conjunto 20 deveria ser confirmado em bancada. [comentário] + [inferido]
