# Jiga: família Connect (Triflex Connect)

Origem: `docs/TesteProgManutencao/`. Lista dos comandos que a jiga envia ao produto e do resultado esperado, para o port da função da jiga para o prog_wifi. Formato dos frames em `docs/jiga-protocol.md`.

Legenda: **[código]** vem do código ativo. **[comentário]** vem de comentário do código. **[inferido]** dedução, precisa de confirmação.

## Resumo

O Connect usa o mesmo teste da Full Range (3 ciclos de ida e volta, controlados pela posição do encoder). O que muda é o **conjunto de comandos do motor**, que depende de como o produto foi identificado: conjunto 20 pelo nome, conjunto 10 pela consulta. O código não diz qual dos dois realmente move o motor. [código] + [comentário]

## Identificação

- Pelo nome: o produto anuncia um nome que contém `"CONNECT"`, por exemplo `TRIFLEX CONNECT v2.34` (chega cortado no fim). Esse teste vem depois de `"Facility"`. Resultado: conjunto 20. [código] + [comentário]
- Pela consulta: o Connect responde ao pedido com ID `'C'` usando ID `'C'`, dentro de 300U. Resultado: conjunto 10. [código]
- A consulta com ID `'L'` só acontece se a resposta ao `'C'` não chegar. Se o `'L'` também não responder e o produto já tinha respondido ao `'C'`, a jiga volta para `'C'` com o conjunto 10. [código]
- O comentário diz que o Facility também responde à consulta `'C'`. Nesse caso a jiga o trata como Full Range ou Connect, com conjunto 10. Pela consulta, Connect e Full Range também não se distinguem. [comentário] + [código]
- As versões `3.15`, `2.34`, `2.36` e `2.45` mudam só o texto do LCD (`TRIFLEX CONNECT TESTE CL`), não o teste. [código]
- Firmware antigo sem nome: se a resposta chega sem nome e com ID `'C'`, a jiga assume Connect. [código]
- Depois de identificar, a jiga espera 30U e, no caminho pelo nome, mais 2 s reais, com uma tela temporária "RECONHECIDO" de 4 s. [código]

## Frame

Pedido: `7E 08 43 ('C') 54 ('T') <CMD> 20 <CKS> 51`. [código]

Bits de `CMD`, pelo significado dos comentários do código **[inferido]**:

| Bit | Valor | Significado |
|---|---|---|
| frente | 0x04 | liga o motor para frente |
| sentido oposto | 0x08 | liga o motor no sentido oposto |
| alta velocidade | 0x10 | seletor de velocidade, não é comando de giro |

`U` é a unidade de espera da jiga. O tempo real de 1U não foi medido (cerca de 15 voltas do laço principal). [código] + [inferido]

## Comandos de um ciclo, conjunto 20 (identificado pelo nome)

| # | Comando | Frame | Quando envia | Espera depois |
|---|---|---|---|---|
| 1 | Frente (0x04) | `7E 08 43 ('C') 54 ('T') 04 20 BA 51` | Primeiro ciclo: depois da espera da identificação. Ciclos seguintes: logo | 254U |
| 2 | Frente + sentido oposto (0x0C) | `7E 08 43 ('C') 54 ('T') 0C 20 B2 51` | Espera concluída e encoder na janela de partida | 254U |
| 3 | Para (0x00) | `7E 08 43 ('C') 54 ('T') 00 20 BE 51` | Espera concluída e encoder na janela de parada da ida | 254U |
| 4 | Alta velocidade (0x10) | `7E 08 43 ('C') 54 ('T') 10 20 AE 51` | Espera concluída | 254U |
| 5 | Alta velocidade + sentido oposto (0x18) | `7E 08 43 ('C') 54 ('T') 18 20 A6 51` | Espera concluída e encoder na janela da volta | 254U |
| 6 | Para (0x00), conta 1 ciclo | `7E 08 43 ('C') 54 ('T') 00 20 BE 51` | Espera concluída e encoder na janela do fim | 254U |
| 7 | Para (0x00) | `7E 08 43 ('C') 54 ('T') 00 20 BE 51` | Espera concluída | 254U, se tiver menos de 3 ciclos. Volta ao comando 1 |

## Comandos de um ciclo, conjunto 10 (identificado pela consulta)

Igual ao conjunto 10 da Full Range (`docs/jiga-familia-full-range.md`): 0x04, 0x14, 0x00, 0x08, 0x18, 0x08 (conta 1 ciclo), 0x00.

O comentário em `TESTERot.c` diz que o conjunto 10 não funciona para o Connect: ele responde à consulta e **para de responder** assim que a alta velocidade (0x10) é ligada. O conjunto 20 é o outro aceito por um produto neste projeto. O resultado em bancada do 20 não está registrado. [comentário]

## Janelas do encoder

A posição é `PlsEncoder` de 16 bits, como na Full Range. O lado A conta de cerca de 0 para cima e o lado B de 65535 para baixo. [código]

| Fase | Lado A | Lado B |
|---|---|---|
| Partida | 50 a 150 | 65450 a 65500 |
| Parada da ida | 700 a 900 | 64650 a 64800 |
| Volta | 500 a 600 | 64900 a 65000 |
| Fim | 10 a 100 | 65400 a 65550 |

## Resultado

- Aprovado (3 ciclos): ` Central Triflex ` / `  Aprovada !!!!` com beep. A jiga fica parada. [código]
- Reprovado: `ERRO ENCODER` / `MOTOR SEM PULSO` com beep, se o motor está comandado (frente ou sentido oposto) e o encoder não muda por 5 s reais. [código]
- Durante o teste, a linha 2 do LCD mostra `ENC:eeeee C:ccc`. [código]

## Falhas

| Sintoma | Significado |
|---|---|
| `ERRO ENCODER` | Motor comandado e encoder sem mudar por 5 s reais. [código] |
| O Connect para de responder logo que o motor liga | É o sintoma descrito no comentário para o conjunto 10. Com o conjunto 20, seria um dado novo. [comentário] + [inferido] |
| Motor não gira, sem erro | Conjunto de comandos errado para o produto, ou o encoder não chega às janelas e o teste fica parado sem mensagem. [inferido] |
| Aprovado depois de 1 ciclo | A contagem de ciclos vinha de uma execução anterior. [código] + [inferido] |

Ao reportar, informar como o produto foi identificado (nome ou consulta), o texto do LCD e a linha `ENC:... C:...`. [inferido]

## Pontos de melhoria e dúvidas

1. O mesmo produto recebe comandos diferentes conforme a identificação (20 pelo nome, 10 pela consulta). Convém decidir qual conjunto vale e eliminar a divergência. [código]
2. O valor do conjunto do Connect está definido duas vezes, com valores diferentes (20 em `TESTERot.c`, 10 em `ModoManutencao.c`). Só o de `TESTERot.c` é usado. [código]
3. Valem aqui os pontos da Full Range: tela temporária de 4 s, falta de limite de tempo por comando, janelas estreitas, contagem de ciclos e lados nunca zerados. Ver `docs/jiga-familia-full-range.md`. 
4. Se o Connect só responde enquanto a alta velocidade está desligada, os comandos 2 e 5 do conjunto 10 derrubam a comunicação. O conjunto 20 deveria ser confirmado em bancada. [comentário] + [inferido]
