# Protocolo UART da jiga de manutenção e detecção da família

Origem: código-fonte em `docs/TesteProgManutencao/` (firmware XMC1100 da jiga de teste de manutenção). Este documento descreve o que a jiga **envia**, o que ela **lê** e como ela **decide a família** do produto. A máquina de estados de teste por família fica em `docs/jiga-state-machine.md`.

Legenda de procedência de cada afirmação:

- **[código]**: decorre diretamente do código executável.
- **[comentário]**: vem de comentário do código (regra do produto ou intenção do autor), sem confirmação no código.
- **[inferido]**: dedução minha a partir de nomes, layout de memória ou analogia. Precisa de confirmação.

Fora de escopo, por pedido: hardware, drivers DAVE, LCD, buzzer, botões (só o papel no fluxo), funções de suporte e qualquer controle de tempo.

Escopo: só o que o código ativo executa e que é alcançável a partir do `main`. Código comentado, estados que nenhum código ativo alcança e campos, bits ou constantes que nada usa foram retirados.

---

## 1. Camada física e de enlace

| Item | Valor | Procedência |
|---|---|---|
| Velocidade / formato | 9600 8N1 | dado fornecido pelo usuário (confirmado em bancada); não aparece em arquivo de código lido |
| Nível | TTL direto do pino do microcontrolador, full-duplex, sem pino de direção | dado do usuário |
| Enlace físico no prog_wifi | A jiga original usa a UART do produto (a "central") diretamente. No prog_wifi essa é a **mesma UART da central que o bridge do seletor intercepta e espelha** (UART2, GPIO17 TX / GPIO16 RX). Seletor, jiga e connect são três dialetos do mesmo enlace, usados um de cada vez. Dado do responsável do projeto | [dado do usuário] |
| Relação | A central está em modo escravo: a jiga é o mestre e o produto só responde a pedido (exceto o banner de identificação, que a central envia em modo mestre, seção 2) | [código] `SCI_TX` e `RX_SCI`; [comentário] banner |
| Ordem de bytes de campos de 16 bits | Big-endian no encoder Triflex (byte [5] = alto, [6] = baixo) | [código] + [inferido] (ver seção 4.3) |

### 1.1 Checksum (todos os frames que têm checksum)

`CKS = ~(b[0] ^ b[1] ^ ... ^ b[n]) & 0xFF`, ou seja, XOR de todos os bytes cobertos, complementado. [código] `CalcCKSt1` (TX) e `CalcCKSr1` (RX), idênticos.

- Pedidos (Triflex e Pop): cobre os bytes `[0..5]`, resultado em `[6]`. [código]
- Resposta Triflex de 17 bytes: cobre `[0..14]`, resultado em `[15]`. [código]
- Resposta Triflex de 19 bytes: cobre `[0..16]`, resultado em `[17]`. [código]
- Resposta Pop: **nenhum checksum é conferido**. [código]

### 1.2 Constantes de enquadramento

| Constante | Valor | Uso | Procedência |
|---|---|---|---|
| `SCI_HEADER` | 126 = 0x7E | Primeiro byte de todo frame | [código] `SCI_tx1.h`, `RX_SCI` |
| `SCI_STOP` | 81 = 0x51 | Último byte do pedido Triflex e da resposta Triflex | [código] |
| `SCI_STOP1` | 0x81 | Último byte do pedido Pop | [código] |
| `LEN_RX` | 0x11 | Tamanho (17) da resposta Triflex padrão | [código] |
| `LEN_RX_IA` | 0x13 | Tamanho (19) da resposta Triflex IA / Facility Axis | [código] |

O byte `[1]` de todo frame é o **comprimento total** do frame em bytes (inclui header, o próprio byte de tamanho, checksum e stop). [código] pedido: `BufferSCI_tx1[1]=8` e `UART_Transmit(..., BufferSCI_tx1[1])` envia exatamente 8 bytes; resposta: 0x11 = 17 e 0x13 = 19 bytes.

### 1.3 Silêncio da jiga e retorno ao modo seletor
- Termos: a central está em **modo mestre** no protocolo do seletor, o único em que ela é o mestre. Nos protocolos da jiga e connect ela está em **modo escravo**: responde aos pedidos de quem manda (a jiga ou o Prog).
- Se a jiga para de pedir, a central sai do modo escravo e volta ao modo mestre (protocolo do seletor) por timeout. Não existe comando de retorno. O mesmo vale para o protocolo connect (`docs/connect-protocol.md`, seção 1): um timeout em qualquer dos dois protocolos reverte a central para o modo mestre. Dado do responsável do projeto.
- O código da jiga não mostra nem testa isso: ela só fica muda durante a escuta do banner e nos estados de fim ou erro, e nunca para de pedir no meio de um teste. O diálogo em lockstep (seção 5, um pedido por resposta válida, retransmissão se a resposta não vier) é o que mantém o produto no protocolo da jiga. [código] + dado do responsável
- O valor do timeout para este protocolo não é conhecido. Para o connect é entre 500 e 1500 ms, conforme o produto.
- Consequências do enlace compartilhado: como a jiga usa a UART2, a mesma do bridge do seletor, o teste da jiga e o bridge não podem usar o enlace ao mesmo tempo. Durante o teste o Prog é o mestre na UART2 e o bridge fica suspenso (sem tradutor, sem encaminhar frames do seletor para a central). O seletor real (UART1) não recebe nada da central nesse período. Ao terminar, o Prog devolve a UART2 ao bridge e a central volta ao modo mestre, quando o bridge retoma o diálogo do seletor.
- Consequências para o port no prog_wifi: o teste não pode ter lacuna entre pedidos (nem durante as esperas do teste, nem por carga de outras tarefas). Depois do fim do teste, aprovado ou não, o Prog fica mudo e espera o primeiro frame válido do seletor vindo da central, como no passo final de salvar preset do connect (`connect-protocol.md` seção 4.1).
- O banner da seção 2 é a tela inicial da central, um frame `'S'` do seletor, que ela repete até o seletor responder (`connect-protocol.md` seção 1). Quando recebe uma resposta válida do seletor, a central passa para a primeira entrada do menu. É o "pop-up" do código do Prog (`selector-bridge-design.md`, seção "Start-screen pop-up and the reset procedure"). Dado do responsável do projeto.
---

## 2. Banner de identificação (frame de texto)

Cada produto transmite o próprio nome em ASCII no power-up. A jiga o usa para decidir a família sem precisar perguntar. [comentário] `TESTERot.c` case 200 ("Todo produto transmite o nome dele no próprio power-up"). Se o produto vai ou não repetir o banner depois não está no código (ver seção 7).
**O banner é a tela inicial da central** (o "pop-up" no código do Prog): o frame `'S'` do seletor com o nome e a versão. A central o repete até receber uma resposta válida do seletor e então passa para a primeira entrada do menu. Dado do responsável do projeto. Ver seção 1.3.

Formato inferido a partir do extrator `VerificaModelo_ExtraiNome`: [código]

| Offset | Conteúdo | Observação |
|---|---|---|
| [0] | 0x7E | Header |
| [1] | Tamanho total `tam` | Aceito se `6 <= tam <= 40` |
| [2] | Byte de ID/tipo | Pode ser imprimível (ex.: `'S'` antes de `Triflex`); é incluído no texto candidato e removido depois pela "limpeza por âncora" |
| [3 .. tam-1] | Texto ASCII do nome e versão | Inclui também os bytes finais do frame (checksum e stop), que entram na contagem de "texto" se forem imprimíveis |

Regras de extração, aplicadas ao buffer bruto de 64 bytes (`ModeloRawBuf`): [código]

1. Varre o buffer procurando `0x7E`. O tamanho `tam` é o byte seguinte. `fim = i + tam`, truncado ao que foi capturado.
2. Conteúdo = bytes de `i+2` até `fim-1`.
3. Caractere de nome válido: `A-Z`, `a-z`, `0-9`, `.`, espaço, `-`, `_`.
4. O frame é aceito como banner se tiver **pelo menos 5 letras** e **pelo menos 3/4 dos bytes do conteúdo** como caractere de nome válido. Frames de status (binários) são ignorados por isso.
5. O nome é a cópia dos caracteres válidos, truncada em 31 caracteres.
6. Limpeza por âncora: procura, nesta ordem de prioridade (não a posição mais cedo), `"Triflex"`, `"TRIFLEX"`, `"AGILITY"`, `"Facility"`. Se achar, descarta tudo antes da âncora. Se nenhuma casar, o nome fica cru.
7. A captura bruta (`VerificaModelo_CapturaByte`) roda para **todo byte recebido**, independente de `ChaveSerial`, até `ModeloIdentificado = 1` ou 64 bytes capturados. [código] `main.c` `EndOfReceiveUART1`.

Exemplos de nome citados em comentários: `TRIFLEX FULL RANGE V3.14`, `TRIFLEX CONNECT v2.34` (chega cortado, sem `.33`), `STriflex Facility Axis`. [comentário]

---

## 3. Frames de pedido (jiga → produto)

### 3.1 Pedido Triflex (Full Range, Connect, Facility, Facility Axis)

Buffer `BufferSCI_tx1`. 8 bytes. [código] `SCI_tx1.c`, case 1.

| Offset | Valor | Significado | Procedência |
|---|---|---|---|
| [0] | 0x7E | Header | [código] |
| [1] | 0x08 | Tamanho total (8) | [código] |
| [2] | `'C'` (0x43) ou `'L'` (0x4C) | ID da família consultada. `'C'` = Full Range ou Connect. `'L'` = Facility (inclui Axis). Quem escolhe a letra é `TESTERot` antes de ligar o TX | [código] |
| [3] | `'T'` (0x54) | Tipo do pedido Triflex, fixo | [código] |
| [4] | `CmdJiga` | Byte de comando (tabela 3.1.1). Reescrito a cada transmissão | [código] |
| [5] | 0x20 | Constante (espaço), nunca alterada | [código] |
| [6] | CKS | XOR de `[0..5]` complementado | [código] |
| [7] | 0x51 | Stop | [código] |

Exemplos calculados (checksum conferido com a fórmula acima):

- `'C'`, CmdJiga = 0x00: `7E 08 43 54 00 20 BE 51`
- `'L'`, CmdJiga = 0x00: `7E 08 4C 54 00 20 B1 51`
- `'C'`, CmdJiga = 0x04 (CMD_FWR): `7E 08 43 54 04 20 BA 51`

#### 3.1.1 Byte `CmdJiga` (campo [4])

Cada bit é um comando. A jiga manipula bit a bit, nunca atribui o byte inteiro, exceto `CmdJiga = 0` ao iniciar a detecção. [código] `types.h`, `TESTERot.c` case 200 e 206.

| Bit | Máscara | Nome | Usado pela jiga | Significado | Procedência |
|---|---|---|---|---|---|
| 2 | 0x04 | `CMD_FWR` | Sim | Motor para frente (liga o giro) | [código] |
| 3 | 0x08 | `CMD_REV` | Sim | Motor em reverso | [código] |
| 4 | 0x10 | `CMD_HIGHsp` | Sim | Seleciona velocidade alta. Não é comando de giro por si só | [código] (comentário em `MotorTriflexComandado`) |

Os bits 0, 1, 5, 6 e 7 não são ligados em nenhum fluxo ativo da jiga.

#### 3.1.2 Conjuntos de polaridade do motor

A jiga tem dois conjuntos de comando de motor, escolhidos na detecção em `IsolaSensorModelosInversores`: [código] `ModoManutencao.c` cases 125 a 130.

Bits ligados em cada giro (cases 125 a 129). [código]

| Valor | Família associada | Primeiro giro (ida) | Segundo giro (volta) |
|---|---|---|---|
| 10 | Full Range | `CMD_FWR` + `CMD_HIGHsp` | `CMD_REV` + `CMD_HIGHsp` |
| 20 | Facility (e Facility Axis) | `CMD_FWR` + `CMD_REV` | `CMD_REV` + `CMD_HIGHsp` (o `CMD_FWR` é zerado antes) |

O detalhe de cada passo, com faixas de encoder, está em `jiga-state-machine.md`. O comentário em `TESTERot.c` diz que os dois conjuntos são espelhos, trocando os bits `CMD_REV` e `CMD_HIGHsp` entre si. [comentário]

O Connect usa um destes conjuntos, e o valor depende do caminho de detecção (seção 6.5). [código]

### 3.2 Pedido Pop / Legero / Agility

Buffer `BufferSCI_tx2`. 8 bytes. [código] `SCI_tx1.c`, case 10.

| Offset | Valor | Significado | Procedência |
|---|---|---|---|
| [0] | 0x7E | Header | [código] |
| [1] | 0x08 | Tamanho total (8) | [código] |
| [2] | `'J'` (0x4A) | ID Pop, fixo | [código] |
| [3] | `CMD1` | Comando principal (tabela 3.2.1) | [código] |
| [4] | `CMD2` | Parâmetro de configuração | [código] |
| [5] | `CMD3` | Nunca atribuído, vale sempre 0 | [código] |
| [6] | CKS | XOR de `[0..5]` complementado | [código] |
| [7] | 0x81 | Stop (`SCI_STOP1`) | [código] |

Exemplo com `CMD1 = 11`, `CMD2 = 0`, `CMD3 = 0`: `7E 08 4A 0B 00 00 C8 81`.

Valor inicial de `CMD1` é 11 (`Ram.c`). `CMD2` e `CMD3` iniciam em 0. [código]

#### 3.2.1 Valores de `CMD1` e `CMD2`

Nenhum arquivo define nomes para estes valores. Os significados abaixo vêm de comentários nos estados de teste e do papel do valor no fluxo. [inferido], salvo indicação.

| Campo | Valor | Onde a jiga usa | Significado provável |
|---|---|---|---|
| CMD1 | 11 | Valor inicial; "estado de repouso" antes e entre giros | Parado / nenhum comando de motor. Comentários: "desliga motor" |
| CMD1 | 1 | Atribuído no case 23 (ao sair para o 24) e no case 51 | Liga o motor. Comentário: "comando motor" / "liga motor alta velocidade" |
| CMD1 | 2 | Cases 25, 26, 31, 53, 54, 58, 59 | Desliga o motor. Comentário: "desliga motor" |
| CMD1 | 0 | Cases 28, 29, 56 | Comando de motor no sentido oposto. Comentário genérico "comando motor"; o código comentado dos cases 128 a 130 diz "sentido contrário" |
| CMD1 | 7 | Cases 32 e 60 | Fim de curso. Comentário: "avança pro fim de curso" |
| CMD1 | 6 | Cases 33 e 61, e no dispatch por modelo | Comando de configuração. A resposta correta é `'O' 'K'` em `[4]`,`[5]` |
| CMD2 | 0 ou 1 | Junto com `CMD1 = 6` | Seleciona variante de configuração por modelo (tabela 6.6) |
| CMD3 | 0 | Sempre | Não usado |

---

## 4. Frames de resposta (produto → jiga)

### 4.1 Máquina de recepção

A recepção é um autômato byte a byte (`RX_SCI` para Triflex, `RX_SCI2` para Pop). `ChaveSerial` escolhe qual roda: 1 = Triflex, 2 = Pop. [código] `main.c` `EndOfReceiveUART1`.

Validação em cada nível: [código]

| Etapa | Triflex (`RX_SCI`) | Pop (`RX_SCI2`) |
|---|---|---|
| Header [0] | Deve ser 0x7E, senão fica na etapa de header | Deve ser 0x7E (o byte é gravado em `[0]` de qualquer forma) |
| Tamanho [1] | Deve ser 0x11 ou 0x13, senão volta ao header | Deve ser 0x0D (13), senão volta ao header |
| ID [2] | Deve ser `'L'`, `'C'` ou 0x00, senão volta ao header | Deve ser `'J'`, senão volta ao header |
| Corpo | Acumula até o fim do tamanho declarado | Acumula até o índice 11 |
| Fechamento | Stop 0x51 **e** checksum conferem | Aceita no 12º byte, sem checksum nem stop |
| Frame rejeitado | Descartado sem evento; quem retransmite é o TX | (não há rejeição após o ID) |

Os dois autômatos gravam no **mesmo** buffer `gBufferSCI_rx1[20]`. [código]

### 4.2 Eventos de detecção levantados pelo RX Triflex

No byte `[2]` (ID) o RX já levanta flags **antes** de validar checksum e stop do frame. [código] `Sci.c` case 3.

| ID recebido | `StartMaquina` | `RespondeuIDC` | Observação |
|---|---|---|---|
| `'L'` (0x4C) | 10 | inalterado | |
| `'C'` (0x43) | 5 | 1 | O comentário diz "NÃO por StartMaquina = 5 aqui", mas a linha existe e é executada. Contradição comentário/código (ver seção 7) |
| 0x00 | 5 | 1 | |
| qualquer outro | inalterado | inalterado | frame descartado, volta ao header |

Portanto `StartMaquina == 5/10` significa "chegou header + tamanho 0x11/0x13 + ID reconhecido", não "frame válido". [código]

### 4.3 Resposta Triflex: 17 bytes (0x11) e 19 bytes (0x13)

Publicada por `PublicaFrameTriflex()` somente se stop e checksum conferem. [código] Todo campo lido de `gBufferSCI_rx1` aparece na tabela.

| Offset | Campo na jiga | Lido? | Significado | Procedência |
|---|---|---|---|---|
| [0] | Header 0x7E | Valida | Início de frame | [código] |
| [1] | Tamanho | Valida e decide a cauda | 0x11 = 17 bytes, 0x13 = 19 bytes | [código] |
| [2] | ID | Valida e decide família no probe | Ver seção 4.2. `'C'`, `'L'` ou 0x00 | [código] |
| [3] | (gravado em `gBufferSCI_rx1[3]`) | **Não publicado**; só entra no checksum | Sem significado conhecido | [código] |
| [5] | `PlsEncoder1` | Sim | Byte **alto** do contador do encoder (ver nota) | [código] |
| [6] | `PlsEncoder0` | Sim | Byte **baixo** do contador do encoder | [código] |
| [4], [7] a [14] | `StJiga0..3`, `AD_Tst_Cp`, `TxJiga` | Gravados em variáveis, sem uso | Nenhum desses campos é lido pelo fluxo ativo | [código] |
| 17 bytes: [15] | CKS | Valida | `~XOR[0..14]` | [código] |
| 17 bytes: [16] | Stop 0x51 | Valida | | [código] |
| 19 bytes: [15] | (coletado) | **Ignorado**, só entra no checksum | Sem significado conhecido | [código] |
| 19 bytes: [16] | (coletado) | **Ignorado**, só entra no checksum | Sem significado conhecido | [código] |
| 19 bytes: [17] | CKS | Valida | `~XOR[0..16]` | [código] |
| 19 bytes: [18] | Stop 0x51 | Valida | | [código] |

**Nota sobre o encoder.** `PlsEncoder` é a união `Var16_2` em que `Byte20` é `unsigned int` e `Byte00`/`Byte01` são os dois primeiros bytes. Em little-endian (Cortex-M0), `Byte00` é o byte baixo e `Byte01` o alto. O código atribui `PlsEncoder0 = [6]` (Byte00, baixo) e `PlsEncoder1 = [5]` (Byte01, alto). Logo `PlsEncoder = ([5] << 8) | [6]`, de 0 a 65535. Os comentários em `types.h` ("Byte mais significativo" para `PlsEncoder0`) dizem o contrário do layout. O código do `ModoManutencao` confirma o resultado: compara contra 65400..65500 no giro em reverso, o que só faz sentido com 16 bits e wrap em 65535. [código] + [inferido]

Os 19 bytes do Facility Axis / Triflex IA têm o mesmo layout de campos `[2..14]`. Só a cauda é maior. [código] comentário em `Sci.c` e a rotina única `PublicaFrameTriflex`.

Em frame de 19 bytes, o ID aceito pelo código continua sendo só `'L'`, `'C'` ou 0x00. Os comentários citam ID `'P'` (0x50) para o Axis ("Triflex IA / Facility Axis (ID 'P')" e "I deve ser 4C ('L') ou 50 ('P')"), mas `'P'` seria **rejeitado** no case 3. [comentário] vs [código]. Ver seção 7.

### 4.4 Resposta Pop / Legero / Agility

Tamanho declarado 0x0D (13 bytes). A jiga **aceita o frame ao receber o 12º byte** (índice 11), sem checksum nem stop. [código] `RX_SCI2` case 12.

| Offset | Campo na jiga | Lido? | Significado | Procedência |
|---|---|---|---|---|
| [0] | Header 0x7E | Valida | | [código] |
| [1] | Tamanho 0x0D | Valida | Declara 13 bytes | [código] |
| [2] | ID `'J'` | Valida; estados 22, 33, 50, 61 testam `[2]=='J'` como "há resposta" | | [código] |
| [3] | gravado | Não lido | Sem significado conhecido | [código] |
| [4] | Confirmação | Sim | `'O'` (0x4F) na resposta ao `CMD1 = 6` (estados 34 e 62). Fora disso, sem uso | [código] |
| [5] | Confirmação | Sim | `'K'` (0x4B) na resposta ao `CMD1 = 6` (estados 34 e 62). Fora disso, sem uso | [código] |
| [6] | gravado | Não lido | Sem significado conhecido | [código] |
| [7] | gravado | Não lido | Sem significado conhecido | [código] |
| [8] | gravado | Não lido | Sem significado conhecido | [código] |
| [9] | `PlsEncoder` (8 bits) | Sim | Contador de encoder de 1 byte. Os estados de teste leem `[9]` direto em `ValorDefineEncA/B` (`unsigned char`) | [código] |
| [10] | gravado | Não lido | Sem significado conhecido | [código] |
| [11] | gravado, fecha o frame | Não lido | Pela analogia com o Triflex seria o checksum | [inferido] |
| [12] | não lido pelo autômato | Nunca lido | Pela analogia com o Triflex seria o stop (0x81) | [inferido] |

A confirmação `'O' 'K'` é testada em `gBufferSCI_rx1[4]` e `[5]` nos cases 34 e 62 do ModoManutencao, depois de enviar `CMD1 = 6`. [código]

---

## 5. Cadência de pedido e resposta (nível de protocolo)

Ignorando temporizações, o diálogo funciona assim. [código] `SCI_TX`

**Triflex** (`CtrlSCI_tx1`):

| Estado | Significado |
|---|---|
| 0 | TX mudo. Usado durante a escuta do banner e nos estados de fim/erro |
| 1 | Monta e transmite um pedido (preenche `[4]=CmdJiga`, CKS, stop). Antes de transmitir, reseta a recepção e rearma `gCtrlSCI_rx1 = 1` e `FrameRx1Completo = 0`. Só transmite se o RX não estiver no meio de um frame (`gCtrlSCI_rx1 <= 1`) |
| 2 | Espera. Se `FrameRx1Completo` (frame válido recebido) volta ao estado 1 imediatamente. Se a resposta não vem, retransmite o pedido (fallback) |

Consequência para a implementação no prog_wifi: **um pedido por resposta válida** (lockstep), e o pedido seguinte já carrega o `CmdJiga` atualizado. Se a resposta não vier, repete o último pedido. O `CmdJiga` é reenviado em todo pedido, então a "taxa de comando" é a taxa de pedido. [código] + [comentário] (`SCI_tx1.c`)

**Pop** (`CtrlSCI_tx1`): 0 = mudo. 10 = transmite pedido (`[3..5] = CMD1..3`). 11 = espera: quando `StartMaquina == 110` (frame Pop aceito) volta ao estado 10; sem resposta também retransmite. [código]

**Como é decidido que cada família tem TX:** a jiga só sai do estado mudo depois de decidir a família (seção 6). Na detecção por banner **não há pedido de identificação**. Na detecção por probe, os pedidos `'C'` e `'L'` com `CmdJiga = 0` fazem o papel de sonda. A família Pop **nunca é sondada com `'J'`**: ela só é reconhecida pelo banner. [código]

---

## 6. Detecção da família

### 6.1 Variáveis de estado envolvidas

| Variável | Quem escreve | Significado | Procedência |
|---|---|---|---|
| `CtrlFunMotor` | `init()` (=200), `TESTERot` | Estado da detecção. Valores usados: 200, 207, 206, 201, 205, 202. 0 = detecção concluída | [código] |
| `ChaveSerial` | `init()` (=1), `TESTERot` | 1 = autômato Triflex; 2 = autômato Pop | [código] |
| `StartMaquina` | `RX_SCI` (5, 10), `RX_SCI2` (110), `TESTERot` (0) | Evento "chegou ID reconhecido" (seção 4.2) | [código] |
| `RespondeuIDC` | `RX_SCI` (1), `TESTERot` (0) | "Alguém respondeu com ID `'C'` ou 0" | [código] |
| `ModeloRawBuf`, `ModeloRawIdx` | `VerificaModelo_CapturaByte` | Buffer bruto de até 64 bytes capturado de tudo que chega | [código] |
| `ModeloNome` | `VerificaModelo_ExtraiNome` | Nome extraído (seção 2) | [código] |
| `ModeloCod` | `VerificaModelo()` | Código de modelo (tabela 6.4) | [código] |
| `IsolaSensorModelosInversores` | `TESTERot`, `ModoManutencao` | 10 = conjunto Full Range; 20 = conjunto Facility | [código] |
| `ChavaModeloPop` | `TESTERot` | 1, 2 ou 5: variante Pop que escolhe o `CMD1/CMD2` de configuração | [código] |

A rotina `TESTERot()` roda primeiro em cada volta do laço principal, e `VerificaModelo()` roda logo depois. [código] `main.c`

### 6.2 Fluxo completo de detecção (`TESTERot`)

```
init(): CtrlFunMotor=200, ChaveSerial=1, CtrlSCI_tx1=0 (mudo)

case 200  (arma a escuta)
   ChaveSerial=1; CtrlSCI_tx1=0 (mudo); gCtrlSCI_rx1=1
   StartMaquina=0; RespondeuIDC=0; CmdJiga=0
   VerificaModelo_Reset()  (zera buffer bruto, ModeloCod, ModeloIdentificado)
   -> case 207

case 207  (escuta do banner, sem transmitir)
   se ModeloRawIdx >= 6:
      VerificaModelo_ExtraiNome(0)
      se nome contém "Facility"      -> letra 'L', polaridade 20
      senão se contém "CONNECT"      -> letra 'C', polaridade POLARIDADE_CONNECT (=20 em TESTERot.c)
      senão se contém "FULL"         -> letra 'C', polaridade 10
      senão se ModeloCod != 0        -> família Pop: ChaveSerial=2, CtrlSCI_tx1=0 -> case 202
   se letra definida (Triflex reconhecido):
      BufferSCI_tx1[2]=letra; IsolaSensorModelosInversores=polaridade
      CtrlSCI_tx1=1; CtrlManutencao=125; CtrlFunMotor=0
   se nada reconhecido e a janela de escuta esgotou -> case 206
```

```
case 206  (fallback: sonda 'C' às cegas)
   BufferSCI_tx1[2]='C'; CtrlSCI_tx1=1; StartMaquina=0; RespondeuIDC=0; CmdJiga=0
   -> case 201

case 201  (espera resposta a 'C')
   se StartMaquina == 5:      (ID 'C' ou 0 recebido)
        CtrlManutencao=125; CtrlFunMotor=0
        se RespondeuIDC: IsolaSensorModelosInversores=10
   senão se RespondeuIDC ou janela esgotou:
        ChaveSerial=1; BufferSCI_tx1[2]='L'; CtrlSCI_tx1=1; StartMaquina=0
        -> case 205

case 205  (espera resposta a 'L')
   se StartMaquina == 10:     (ID 'L' recebido)
        CtrlManutencao=125; IsolaSensorModelosInversores=20; CtrlFunMotor=0
   senão se janela esgotou:
        se RespondeuIDC:      (respondeu 'C' antes, não respondeu 'L')
             BufferSCI_tx1[2]='C'; CtrlSCI_tx1=1
             CtrlManutencao=125; IsolaSensorModelosInversores=10; CtrlFunMotor=0
        senão:                (ninguém respondeu: tenta Pop)
             ChaveSerial=2; CtrlSCI_tx1=0; gCtrlSCI_rx2=1; StartMaquina=0 -> case 202

case 202  (despacho Pop por ModeloCod, depois de esperar o produto inicializar)
   ver tabela 6.6
   se nenhum comparador casa: TempoMaquina=50; CtrlFunMotor=200 (recomeça a detecção)
```

[código] `TESTERot.c`, texto resumido. Janelas e esperas são temporizações e ficam fora deste documento (ver `jiga-state-machine.md`).

Pontos relevantes do fluxo:

- **Escuta primeiro, fala depois.** O caminho principal é por banner, sem enviar nada. [comentário] case 200
- **Probe cego é fallback.** Serve para firmware que não anuncia o nome (o comentário cita o Connect v3.15.33). [comentário] case 206 e `VerificaModelo.c`
- **Reconhecimento Triflex por palavra-chave no nome cru, não por `ModeloCod`.** Dois motivos no comentário: `ModeloCod` só é preenchido por `VerificaModelo()`, que roda depois do `TESTERot` no laço, e a Full Range não tem entrada na tabela. [comentário] case 207
- **Pop depende de `ModeloCod`.** Como `VerificaModelo()` roda depois de `TESTERot()` na mesma volta, a primeira passagem pode não ver `ModeloCod`; a decisão sai na volta seguinte. [código] + [inferido]
- A decisão pelo banner reaproveita o buffer bruto. `VerificaModelo()` continua rodando em paralelo e **só afeta LCD e `ModeloCod`**. [código]

### 6.3 Papel de `StartMaquina`, `RespondeuIDC` e `ChaveSerial`

| Variável | Papel na detecção |
|---|---|
| `StartMaquina` | `5` = ID `'C'` ou 0 chegou (Full Range ou Connect, e o Facility segundo o comentário). `10` = ID `'L'` chegou (Facility). `110` = frame Pop aceito, usado só pelo TX Pop. É zerado pelo `TESTERot` antes de cada probe. No Triflex o valor carrega o ID e por isso o Pop usa um valor distinto (110) |
| `RespondeuIDC` | Sobe com ID `'C'` **e também com ID 0**. O comentário do `Ram.c` e do case 201 diz que sobe só com `'C'`, mas o código sobe nos dois casos. É a memória "alguém respondeu 'C'", usada como desempate no case 205 |
| `ChaveSerial` | Chave de despacho do autômato RX (1 Triflex, 2 Pop). Fica em 2 depois que a detecção cai no Pop. Só volta a 1 quando a detecção reinicia (case 200 ou 201 a 205) |

[código] + [comentário]

### 6.4 Tabela de códigos de modelo (`VerificaModelo.h`) e família

`VerificaModelo()` compara o nome extraído com `ModeloTabela` via `strstr`, **na ordem da tabela**, e usa o primeiro que casar. [código] Entradas por texto exato (inclusive espaços de enchimento, que fazem parte da string procurada).

| Código | Constante | Família | Como é atribuído | Observação |
|---|---|---|---|---|
| 0 | `MODELO_DESCONHECIDO` | n/d | Padrão | |
| 1 | `MODELO_TRIFLEX` | Facility (Triflex ID `'L'`) | `StartMaquina == 10` (probe `'L'` respondido) | Mostra "Triflex Facility      V2.1" no LCD. Não vem da tabela |
| 2 | `MODELO_AGILITY_LEGERO_V2_3` | Pop/Legero | Tabela: `"AGILITY LEGERO  V2.3"` | |
| 3 | `MODELO_AGILITY_CANCELA_V1_3` | Agility Cancela (Pop) | Tabela: `"AGILITY CANCELA V1.3"`, e também V1.4, V1.5, V1.6 (todas mapeadas para 3) | **Sem despacho**: o comparador de Cancela está comentado em `case 202`. Ver seção 7 |
| 4 | `MODELO_Triflex_Facility______V2_1` | Facility | Tabela: `"Triflex Facility      V2.1"` | O despacho do case 202 vai para `CtrlManutencao = 190`, que não existe no ModoManutencao ativo. Na prática o Facility é tratado antes, pela palavra `"Facility"` no case 207 |
| 6 | `MODELO_AGILITY_SP______V1_0` | Agility SP (Pop) | Tabela: `"AGILITY SP      V1.0"` | |
| 7 | `MODELO_AGILITY_HIBRIDA_V1_4` | Agility Híbrida (Pop) | Tabela: `"AGILITY HIBRIDA V1.4"` | |
| 8 | `MODELO_AGILITY_HIBRIDA_V1_5` | Agility Híbrida | Tabela: `"AGILITY HIBRIDA V1.5"` | |
| 9 | `MODELO_AGILITY_HIBRIDA_V1_6` | Agility Híbrida | Tabela: `"AGILITY HIBRIDA V1.6"` | |
| 10 | `MODELO_AGILITY_HIBRIDA_V1_7` | Agility Híbrida | Tabela: `"AGILITY HIBRIDA V1.7"` | |
| 14 | `MODELO_AGILITY_SP______V1_1` | Agility SP | Tabela: `"AGILITY SP      V1.1"` | |
| 15 | `MODELO_AGILITY_SP______V1_2` | Agility SP | Tabela: `"AGILITY SP      V1.2"` | |
| 16 | `MODELO_AGILITY_SP______V1_3` | Agility SP | Tabela: `"AGILITY SP      V1.3"` | |
| 17 | `MODELO_AGILITY_LEGERO_V2_4` | Pop/Legero | Tabela: `"AGILITY LEGERO  V2.4"` | |
| 18 | `MODELO_AGILITY_LEGERO_V2_5` | Pop/Legero | Tabela: `"AGILITY LEGERO  V2.5"` | |
| 19 | `MODELO_AGILITY_LEGERO_V2_6` | Pop/Legero | Tabela: `"AGILITY LEGERO  V2.6"` | |
| 20 | `MODELO_AGILITY__AXIS___V1_0` | Agility Axis (Pop) | Tabela: `"AGILITY  AXIS   V1.0"` | Não confundir com o Triflex Facility Axis (seção 6.7) |
| 21 | `MODELO_AGILITY__AXIS___V1_1` | Agility Axis (Pop) | Tabela: `"AGILITY  AXIS   V1.1"` | |
| 22 | `MODELO_AGILITY__AXIS___V1_2` | Agility Axis (Pop) | Tabela: `"AGILITY  AXIS   V1.2"` | **Sem despacho**: o comparador do case 202 lista V1_0 e V1_1 (V1_1 duas vezes) e omite V1_2 |
| 23 | `MODELO_TRIFLEX_CONNECT_V3_15_33` | Connect (Triflex `'C'`) | Nome contém `"3.15"`; ou, no probe, `StartMaquina == 5` com buffer cheio e sem nome (ID 0, firmware antigo) | Não está em `ModeloTabela` |
| 24 | `MODELO_TRIFLEX_CONNECT_V3_34_33` | Connect | Nome contém `"2.34"`; ou ID `'C'` com buffer cheio e nome sem `"Facility"` | A chave `"2.34"` devolve o código `V3_34`; o nome do define não bate com a chave |
| 25 | `MODELO_TRIFLEX_CONNECT_V3_36_33` | Connect | Nome contém `"2.36"` | |
| 26 | `MODELO_TRIFLEX_CONNECT_V2_45_33` | Connect | Nome contém `"2.45"` | |

Os quatro códigos 23 a 26 mostram a mesma tela no LCD e **não alteram o fluxo de teste**: o Connect é reconhecido pelo `TESTERot` (`"CONNECT"` no nome, ou probe), não por esses códigos. [código]

Ordem de verificação em `VerificaModelo()`: [código]

1. Se `StartMaquina == 5` ou `RespondeuIDC`: tenta a variante Connect pelo nome (seção 6.4, códigos 23 a 26). Se o nome ainda não chegou e o buffer encheu (63 bytes ou mais): `StartMaquina == 5` vira 23; ID `'C'` com nome que não contém `"Facility"` vira 24.
2. Se `StartMaquina == 10`: `MODELO_TRIFLEX` (1).
3. Senão, se há 6 bytes ou mais: extrai o nome, compara com a tabela. Se não casa, espera o buffer encher e então mostra o nome cru, com `ModeloCod = 0`.

### 6.5 Critérios de decisão por família

| Família | Critério principal (banner) | Critério fallback (probe) | Resultado |
|---|---|---|---|
| Full Range | Nome contém `"FULL"` (e não `"Facility"` nem `"CONNECT"`) | Probe `'C'` respondido com ID 0 e `RespondeuIDC` | ID de pedido `'C'`, polaridade 10 |
| Connect | Nome contém `"CONNECT"` (e não `"Facility"`) | Probe `'C'` respondido com ID `'C'`, ou (após `'L'` sem resposta) `RespondeuIDC` | ID de pedido `'C'`. **Polaridade 20 via banner** (`POLARIDADE_CONNECT` em TESTERot.c) e **polaridade 10 via probe** (valor fixo 10 nos cases 201 e 205) |
| Facility | Nome contém `"Facility"` (testado primeiro) | Probe `'L'` respondido com ID `'L'` (`StartMaquina == 10`) | ID de pedido `'L'`, polaridade 20 |
| Facility Axis / Triflex IA | Nome com `"Facility"` (ex.: `"STriflex Facility Axis"`), igual ao Facility | ID `'L'` | Igual ao Facility. A única diferença visível ao código é o frame de resposta de 19 bytes |
| Pop/Legero, Agility (Híbrida, SP, Legero, Axis, Cancela) | Nome casa em `ModeloTabela` com `ModeloCod != 0` e não contém as palavras Triflex acima | Probes `'C'` e `'L'` sem resposta, ainda assim depende do banner (`ModeloCod`) | `ChaveSerial = 2`, despacho do case 202 |

[código] A ordem de teste das palavras no case 207 é: `"Facility"`, `"CONNECT"`, `"FULL"`, depois `ModeloCod`. Um nome que contenha mais de uma palavra segue a primeira que casar.

O Connect tem comportamento diferente conforme o caminho (polaridade 20 pelo banner, 10 pelo probe). O comentário de `TESTERot.c` diz que 20 é a configuração atual e que o 10 "não funciona", enquanto o ModoManutencao define seu próprio `POLARIDADE_CONNECT 10`, que não é usado nos trechos ativos. [código] + [comentário]

### 6.6 Despacho Pop (case 202)

Executado quando a detecção cai na família Pop. Todos os despachos ligam o TX Pop e a recepção. [código]

| Famílias (`ModeloCod`) | `CtrlManutencao` | `ChavaModeloPop` | `CMD1` | `CMD2` |
|---|---|---|---|---|
| Híbrida V1.4 a V1.7 (7, 8, 9, 10) e Axis V1.0, V1.1 (20, 21) | 22 | 1 | 6 | inalterado |
| Legero V2.3 a V2.6 (2, 17, 18, 19) | 22 | 2 | 6 | 1 |
| SP V1.0 a V1.3 (6, 14, 15, 16) | 50 | não usado | inalterado | inalterado |
| Facility V2.1 (4) | 190 (sem case ativo) | inalterado | inalterado | inalterado |
| Cancela (3), Axis V1.2 (22), Triflex (1), Connect (23 a 26), desconhecido (0) | nenhum despacho (recomeça a detecção no case 200) | | | |

Em todos os despachos que casam: `CtrlSCI_tx1 = 10`, `gCtrlSCI_rx2 = 1`, `StartMaquina = 0`, `CtrlFunMotor = 0`. [código]

`CMD1`/`CMD2` de configuração final (ModoManutencao case 33, por `ChavaModeloPop`): 1 = `CMD1 = 6`; 2 = `CMD1 = 6`, `CMD2 = 1`. O caminho SP (case 61) usa `CMD1 = 6` sem `CMD2`. [código]

### 6.7 Distinção "Triflex Facility Axis" x "Agility Axis"

São produtos diferentes no código:

- **Triflex Facility Axis / Triflex IA**: protocolo Triflex, resposta de 19 bytes, nome com `"Facility"`, pedido com ID `'L'`. [código] + [comentário]
- **Agility Axis V1.0 a V1.2**: protocolo Pop (`'J'`), códigos 20 a 22. [código]

---

## 7. Lacunas e perguntas que o código não responde

Marcadas como **L** (lacuna) ou **C** (contradição).

### Enlace e framing

1. **L**: velocidade, paridade e stop da UART não estão em nenhum arquivo lido. O 9600 8N1 vem só do dado do usuário. Os parâmetros ficam nos arquivos do DAVE, fora do escopo.
2. **L**: se o produto envia o banner **apenas no power-up** ou também periodicamente ou sob pedido. O código só assume o power-up. Parcialmente respondida: o banner é a tela inicial da central e ela a repete até o seletor responder, inclusive depois de um timeout da jiga (seção 1.3). Falta saber se a central aceita pedidos da jiga a qualquer momento ou só depois do power-up (`open-questions.md`, J3).
3. **L**: formato exato do banner: significado do byte `[2]` (tipo/ID), comprimento real por produto e posição exata do nome. O extrator tolera `[2]` imprimível. O banner é um frame `'S'` do seletor (tela inicial da central), então o formato é o de `selector-protocol.md`; falta conferir isso contra uma captura.
4. **L**: se um banner de tamanho 0x11 ou 0x13 com `[2]` igual a `'C'`, `'L'` ou 0 acionaria o `RX_SCI` como se fosse resposta (o RX Triflex roda em paralelo à captura bruta durante a escuta).
5. **L**: bytes que a jiga não interpreta e o produto envia: Triflex `[3]`, `[10]` (`AD_Tst_Cp`), `[11..14]` (`TxJiga`), e no frame de 19 bytes `[15]` e `[16]`. Pop `[3]`, `[6]`, `[7]`, `[8]`, `[10]`, `[11]`, `[12]`.
6. **L**: o frame Pop declara 13 bytes, mas o autômato fecha no 12º. O que o 13º byte (e o 12º) significam não é conferido. Pela analogia com o Triflex seriam checksum e stop.
7. **L**: significado de cada valor de `CMD1` e `CMD2` do Pop. Não há tabela de definições; os significados da seção 3.2.1 vêm de comentários e do contexto.
8. **L**: a unidade e a escala do encoder (pulsos por volta) e se o encoder Pop de 8 bits sofre wrap. Os testes de motor do Pop só comparam `[9]` antes e depois (`!=`).
9. **L**: o buffer `gBufferSCI_rx1` é compartilhado e sobrescrito a cada byte. Os estados Pop leem `[2]`, `[4]`, `[5]`, `[9]` direto do buffer sem garantir que pertencem a um frame completo e recente.

### Detecção

10. **C**: o comentário do `Sci.c` ("NÃO por StartMaquina = 5 aqui") diz que o ID `'C'` não deve levantar `StartMaquina = 5`, mas a linha `StartMaquina = 5` está lá e é executada. Efeito: no probe, o case 201 captura qualquer produto que responda ao `'C'` (inclusive o Facility, segundo o comentário) como Full Range/Connect antes de tentar o `'L'`. O ramo "alguém respondeu 'C' mas não 'L'" do case 205 só é alcançável por resposta tardia.
11. **C**: `RespondeuIDC` é descrito como "só sobe no 'C'", mas o código também o levanta para ID 0. Por isso o `if(RespondeuIDC)` do case 201 é sempre verdadeiro quando `StartMaquina == 5`.
12. **C**: ID `'P'` do Facility Axis nos comentários, porém o case 3 do `RX_SCI` rejeita `'P'`. Não está claro se o Axis responde `'L'` (aceito) ou `'P'` (descartado).
13. **C**: o Connect usa polaridade 20 via banner e 10 via probe. Não há explicação de qual conjunto realmente funciona, e os comentários indicam teste em bancada sem resultado registrado.
14. **L**: o comparador Cancela do case 202 está comentado, então Cancela (código 3) não tem teste e a detecção recomeça. O mesmo vale para Axis V1.2 (omitido do comparador, enquanto V1.1 aparece duas vezes). Não se sabe se é omissão intencional.
15. **L**: o despacho do Facility V2.1 (código 4) do case 202 aponta para `CtrlManutencao = 190`, mas o `case 190` do ModoManutencao está comentado. Se esse caminho for alcançável, a máquina para sem estado ativo.
16. **L**: o nome do Facility Axis não casa com a entrada `"Triflex Facility      V2.1"` (6 espaços); o reconhecimento depende só da palavra `"Facility"`.
17. **L**: nomes exatos transmitidos por cada produto (espaçamento incluído) só aparecem em `ModeloTabela` e nos comentários. As strings de tabela com espaços de enchimento (`"AGILITY LEGERO  V2.3"`, `"AGILITY  AXIS   V1.0"`) dependem de o produto enviar exatamente esses espaços.
18. **L**: `CMD2` não é reiniciado entre testes. Híbrida e SP herdam o `CMD2` do teste anterior (Legero deixa 1).
19. **L**: valor do timeout que faz a central voltar ao modo mestre quando a jiga para de pedir (seção 1.3), por produto, e o que o reinicia (qualquer pedido válido ou só alguns). Ver `open-questions.md`, J1.
20. **L**: o que o motor faz quando o timeout ocorre com um comando de giro ativo: para ou mantém o último comando. Ver `open-questions.md`, J2.

### Observação sobre o que foi lido

Lidos por inteiro: `Sci.c`, `SCI_tx1.c`/`.h`, `Sci.h`, `VerificaModelo.c`/`.h`, `TESTERot.c`, `ModoManutencao.c`, `Flash.c`, `Defines.h`, `types.h`, `Ram.c`, `Ram.h`, `main.c`, `init.c`. Lidos em parte: `Counter.c` (trecho de decrementos e inicialização de Pop), `Botao_centro.c`, `README.md`. Não lidos: drivers DAVE, LCD, buzzer, letreiro, demais botões, `EncontraProduto.c` (apenas por busca), `Flash.c.bak`, `solver.*`.
