# Jiga: família Full Range

Origem: `docs/TesteProgManutencao/`. Lista dos comandos que a jiga envia ao produto e do resultado esperado, para o port da função da jiga para o prog_wifi. Formato dos frames em `docs/jiga-protocol.md`.

Legenda: **[código]** vem do código ativo. **[comentário]** vem de comentário do código. **[inferido]** dedução, precisa de confirmação.

## Resumo

Triflex Full Range, conjunto de comandos 10. A jiga liga o motor para frente, para na posição lida no encoder, liga o sentido oposto, para perto do ponto de partida, e repete até completar 3 ciclos. Cada passo depende da posição do encoder (16 bits) ou de uma espera. A única reprovação é `ERRO ENCODER`. [código]

## Identificação

- O produto anuncia um nome que contém `"FULL"`, por exemplo `TRIFLEX FULL RANGE V3.14`. A jiga escuta o nome por até 4 s. Esse teste vem depois dos nomes `"Facility"` e `"CONNECT"`. [código]
- Sem nome reconhecido, a jiga pede com ID `'C'`. Resposta com ID `0` (Full Range) dentro de 300U identifica o produto. [código] + [comentário]
- O Connect responde com ID `'C'` e cai no mesmo caminho, com o mesmo conjunto de comandos. O código ativo não os separa pelo probe. [código]
- Depois de identificar, a jiga espera 30U e pelo menos 2 s reais antes de ligar o motor. No caminho pelo nome há ainda uma tela temporária "RECONHECIDO" de 4 s. [código]

## Frame

Pedido: `7E 08 43 ('C') 54 ('T') <CMD> 20 <CKS> 51`. [código]

Bits de `CMD`, pelo significado dos comentários do código **[inferido]**:

| Bit | Valor | Significado |
|---|---|---|
| frente | 0x04 | liga o motor para frente |
| sentido oposto | 0x08 | liga o motor no sentido oposto |
| alta velocidade | 0x10 | seletor de velocidade, não é comando de giro |

Com todos os bits em 0, o frame é `7E 08 43 ('C') 54 ('T') 00 20 BE 51`.

## Esperas

`U` é a unidade de espera da jiga. O tempo real de 1U não foi medido. Pelo cálculo do código, 1U equivale a cerca de 15 voltas do laço principal. [código] + [inferido]

## Janelas do encoder

A posição é `PlsEncoder` = byte `[5]` x 256 + byte `[6]` da resposta. O lado A conta a partir de cerca de 0 para cima. O lado B conta a partir de 65535 para baixo. Os limites são estritos (`>` e `<`). [código] + [inferido]

| Fase | Lado A | Lado B |
|---|---|---|
| Partida | 50 a 150 | 65450 a 65500 |
| Parada da ida | 700 a 900 | 64650 a 64800 |
| Volta | 500 a 600 | 64900 a 65000 |
| Fim | 10 a 100 | 65400 a 65550 |

Na parada da ida e na volta, o lado precisa ter sido marcado antes. A distância de ida é de cerca de 700 a 900 contagens a partir da posição inicial. [código] + [inferido]

## Comandos de um ciclo

| # | Comando | Frame | Quando envia | Espera depois |
|---|---|---|---|---|
| 1 | Frente (0x04) | `7E 08 43 ('C') 54 ('T') 04 20 BA 51` | Primeiro ciclo: depois da espera da identificação e com o produto pronto. Ciclos seguintes: logo | 254U |
| 2 | Frente + alta velocidade (0x14) | `7E 08 43 ('C') 54 ('T') 14 20 AA 51` | Espera concluída e encoder na janela de partida | 254U |
| 3 | Para (0x00) | `7E 08 43 ('C') 54 ('T') 00 20 BE 51` | Espera concluída e encoder na janela de parada da ida | 254U |
| 4 | Sentido oposto (0x08) | `7E 08 43 ('C') 54 ('T') 08 20 B6 51` | Espera concluída | 254U |
| 5 | Sentido oposto + alta velocidade (0x18) | `7E 08 43 ('C') 54 ('T') 18 20 A6 51` | Espera concluída e encoder na janela da volta | 254U |
| 6 | Sentido oposto (0x08), conta 1 ciclo | `7E 08 43 ('C') 54 ('T') 08 20 B6 51` | Espera concluída e encoder na janela do fim | 254U |
| 7 | Para (0x00) | `7E 08 43 ('C') 54 ('T') 00 20 BE 51` | Espera concluída | 254U, se tiver menos de 3 ciclos. Volta ao comando 1 |

Resultado esperado: o encoder passa pelas janelas na ordem dos comandos. Se o encoder não entrar na janela, a jiga espera sem limite de tempo e o motor continua comandado. [código]

## Resultado

- Aprovado (3 ciclos contados): o LCD mostra ` Central Triflex ` / `  Aprovada !!!!` com beep. O TX Triflex continua pedindo e a jiga fica parada. [código]
- Reprovado: `ERRO ENCODER` / `MOTOR SEM PULSO` com beep. Acontece quando o motor está comandado (frente ou sentido oposto) e o encoder não muda por 5 s reais. [código]
- Nada reinicia a detecção depois do fim. Para testar outro produto, a jiga precisa ser reiniciada. [código] + [inferido]
- Durante o teste, a linha 2 do LCD mostra `ENC:eeeee C:ccc` (encoder e ciclos contados). [código]

## Falhas

| Sintoma | Significado |
|---|---|
| `ERRO ENCODER` | Motor parado, encoder sem pulso, ou frames de resposta deixando de chegar (o valor só atualiza com frame válido). [código] + [inferido] |
| Motor girando e teste parado | O encoder passou pela janela entre duas leituras, ou nunca entrou nela. Não há limite de tempo nem erro. [código] + [inferido] |
| Nome no LCD e teste não começa | O motor não foi ligado: a espera da identificação, a indicação de produto pronto ou o conjunto de comandos ainda não tem valor. [código] + [inferido] |
| Detecção repetindo | Nenhuma família respondeu ao nome nem às consultas. [código] |
| Aprovado depois de 1 ciclo | A contagem de ciclos vinha de uma execução anterior. [código] + [inferido] |

## Pontos de melhoria e dúvidas

1. A tela temporária de 4 s ("MEDIÇÃO TEMPORÁRIA - REMOVER DEPOIS") bloqueia o laço e anula a espera real de 2 s no caminho pelo nome. [código] + [comentário]
2. Falta um limite de tempo por comando. Hoje só o vigia do encoder, que não pega o caso de encoder em movimento fora das janelas. [código] + [inferido]
3. As janelas são estreitas (50 a 200 contagens) e dependem de a leitura cair dentro delas. [código]
4. O conjunto 10 grava o lado A também no caminho do lado B (provável cópia e cola), e os lados nunca são zerados. Depois do primeiro ciclo as janelas dos dois lados ficam válidas. [código] + [inferido]
5. A contagem de ciclos não é reiniciada entre execuções. [código]
6. No comando 6 o sentido oposto continua ligado até o comando 7. [código]
7. Não se sabe a escala do encoder (contagens por volta) nem a velocidade com que ele passa pelas janelas. Esperas em tempo real, e não em voltas do laço, tornariam o teste previsível.
