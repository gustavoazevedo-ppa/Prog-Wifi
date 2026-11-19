# Jiga: família Facility (Triflex Facility e Facility Axis)

Origem: `docs/TesteProgManutencao/`. Lista dos comandos que a jiga envia ao produto e do resultado esperado, para o port da função da jiga para o prog_wifi. Formato dos frames em `docs/jiga-protocol.md`.

Legenda: **[código]** vem do código ativo. **[comentário]** vem de comentário do código. **[inferido]** dedução, precisa de confirmação.

## Resumo

Triflex Facility, conjunto de comandos 20, pedido com ID `'L'`. O teste é o mesmo da Full Range: 3 ciclos de ida e volta, controlados pela posição do encoder. O Facility Axis (Triflex IA) segue exatamente o mesmo teste. A única diferença visível para a jiga é a resposta de 19 bytes em vez de 17. [código]

## Identificação

- Pelo nome: o produto anuncia um nome que contém `"Facility"`, por exemplo `STriflex Facility Axis`. É o **primeiro** nome testado. [código] + [comentário]
- Pela consulta: a jiga pede primeiro com `'C'`. Só se nenhuma resposta chegar em 300U, pede com `'L'`. Resposta com ID `'L'` identifica o Facility. [código]
- O comentário diz que o Facility também responde ao `'C'`. Se responder, a jiga o trata como Full Range ou Connect, com o conjunto 10. Pelo nome isso não acontece. [comentário] vs [código]
- Os comentários citam o ID `'P'` para o Axis, mas a jiga só aceita respostas com ID `'L'`, `'C'` ou `0`. Se o Axis responder `'P'`, o frame é descartado. [comentário] vs [código]
- Depois de identificar, a jiga espera 30U e, no caminho pelo nome, mais 2 s reais, com uma tela temporária "RECONHECIDO" de 4 s. [código]

## Frame

Pedido: `7E 08 4C ('L') 54 ('T') <CMD> 20 <CKS> 51` (ID `'L'`). [código]

Bits de `CMD`, pelo significado dos comentários do código **[inferido]**:

| Bit | Valor | Significado |
|---|---|---|
| frente | 0x04 | liga o motor para frente |
| sentido oposto | 0x08 | liga o motor no sentido oposto |
| alta velocidade | 0x10 | seletor de velocidade, não é comando de giro |

Com todos os bits em 0, o frame é `7E 08 4C ('L') 54 ('T') 00 20 B1 51`.

`U` é a unidade de espera da jiga. O tempo real de 1U não foi medido (cerca de 15 voltas do laço principal). [código] + [inferido]

## Comandos de um ciclo

| # | Comando | Frame | Quando envia | Espera depois |
|---|---|---|---|---|
| 1 | Frente (0x04) | `7E 08 4C ('L') 54 ('T') 04 20 B5 51` | Primeiro ciclo: depois da espera da identificação. Ciclos seguintes: logo | 254U |
| 2 | Frente + sentido oposto (0x0C) | `7E 08 4C ('L') 54 ('T') 0C 20 BD 51` | Espera concluída e encoder na janela de partida | 254U |
| 3 | Para (0x00) | `7E 08 4C ('L') 54 ('T') 00 20 B1 51` | Espera concluída e encoder na janela de parada da ida | 254U |
| 4 | Alta velocidade (0x10) | `7E 08 4C ('L') 54 ('T') 10 20 A1 51` | Espera concluída | 254U |
| 5 | Alta velocidade + sentido oposto (0x18) | `7E 08 4C ('L') 54 ('T') 18 20 A9 51` | Espera concluída e encoder na janela da volta | 254U |
| 6 | Para (0x00), conta 1 ciclo | `7E 08 4C ('L') 54 ('T') 00 20 B1 51` | Espera concluída e encoder na janela do fim | 254U |
| 7 | Para (0x00) | `7E 08 4C ('L') 54 ('T') 00 20 B1 51` | Espera concluída | 254U, se tiver menos de 3 ciclos. Volta ao comando 1 |

O comando 2 liga o sentido oposto **sem** desligar a frente, então os dois bits ficam em 1 até o comando 3. O conjunto 20 é o espelho do conjunto 10 da Full Range: trocam o sentido oposto e a alta velocidade. [código] + [comentário]

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
- Reprovado: `ERRO ENCODER` / `MOTOR SEM PULSO` com beep, se o motor está comandado com frente ou sentido oposto e o encoder não muda por 5 s reais. [código]
- No comando 4 só a alta velocidade está ligada. A jiga não conta isso como motor comandado, então nesse comando não há proteção contra encoder parado. [código] + [inferido]
- Durante o teste, a linha 2 do LCD mostra `ENC:eeeee C:ccc`. Quando identificado pela consulta, a tela mostra `Triflex Facility      V2.1`. [código]

## Falhas

| Sintoma | Significado |
|---|---|
| `ERRO ENCODER` | Motor comandado e encoder sem mudar por 5 s reais. [código] |
| Detecção repete e o Axis não é encontrado | O Axis pode estar respondendo com ID `'P'`, que a jiga rejeita. [comentário] + [inferido] |
| Teste parado com o motor girando | O encoder passou pela janela entre leituras, ou nunca entrou nela. Não há limite de tempo nem erro. [código] + [inferido] |
| Aprovado depois de 1 ciclo | A contagem de ciclos vinha de uma execução anterior. [código] + [inferido] |

Ao reportar, informar o texto do LCD e a linha `ENC:... C:...`. [inferido]

## Pontos de melhoria e dúvidas

1. No comando 6, o conjunto 20 grava os lados A e B de forma assimétrica (no lado A grava B, no lado B grava B). Provável cópia e cola. Os lados nunca são zerados. [código] + [inferido]
2. A tela temporária de 4 s e os demais pontos da Full Range também valem aqui. Ver `docs/jiga-familia-full-range.md`.
3. O ID `'P'` do Axis nos comentários precisa ser conferido com a equipe do produto.
4. O comando 4 deixa o motor comandado só com a alta velocidade, sem que a jiga veja isso como motor em movimento. [código] + [inferido]
