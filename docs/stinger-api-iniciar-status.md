## Visão geral

1. O Stinger envia `POST /iniciar` com os dados do produto e o caminho base dos presets.
2. O Prog baixa o arquivo `exemplo.conf`, valida o conteúdo e armazena em memória. **A resposta do POST volta assim que o preset está em memória.** A transferência por UART e o teste rodam em uma task separada, sem travar a resposta. O andamento e o resultado são acompanhados somente pelo `GET /status` (`status: 1` enquanto transfere e testa).
3. O Stinger consulta `GET /status` a cada X segundos até o `status` ser `2` ou `3`.

```
Stinger                                   Prog
   |  POST /iniciar {id, produto, ...}      |
   |--------------------------------------->|  baixa e valida exemplo.conf
   |  200 {"id":1234,"status":1,...}        |
   |<---------------------------------------|
   |  GET /status  (a cada X segundos)      |
   |--------------------------------------->|
   |  200 {"id":1234,"status":1|2|3,...}    |
   |<---------------------------------------|
```

## Dados de conexão

| Item | Valor |
|------|-------|
| URL base | `http://{IP_DO_PROG}` (porta 80) |
| Autenticação | Header `X-API-Key: abcdef12345XYZ` (obrigatório nos dois endpoints) |
| Content-Type da resposta | `application/json` (com `Content-Length`, sem chunked, ASCII) |
| Timeout recomendado no cliente do `POST` | **acima de 30 s** (pior caso: 3 URLs x 5 s de timeout cada). A transferência por UART e o teste não contam, pois o POST não espera por eles. |

Observações:
- O nome do header é case-insensitive. O valor da chave é comparado exatamente.
- Chave errada ou ausente: `401` com corpo vazio.
- A chave é uma constante no firmware (`PRESET_API_KEY`). Se for trocada no código, este documento deve ser atualizado.

---

## 1. `POST /iniciar`

Inicia uma execução de teste.

### Corpo da requisição (JSON válido)

```json
{
  "id": 1234,
  "produto": "F02303100",
  "cliente": "008249",
  "estado": "SP",
  "caminho": "https://preset.ppa.com.br/agility123/"
}
```

| Campo | Tipo | Obrigatório | Descrição |
|-------|------|-------------|-----------|
| `id` | número ou string numérica | **Sim** | Identificador da execução. É devolvido igual no `/status`. `1234` e `"1234"` são aceitos, e o tipo enviado é o tipo devolvido. |
| `caminho` | string | **Sim** | URL base dos presets. Deve começar com `http://` ou `https://`. A `/` final é adicionada se faltar. |
| `cliente` | string | Não | Código do cliente. Usado na 1ª busca. Vazio ou ausente pula essa busca. |
| `estado` | string | Não | UF. Usado na 2ª busca. Vazio ou ausente pula essa busca. |
| `produto` | string | Não | Apenas informativo (aparece no log e no histórico). |

Regras de tolerância:
- Campos desconhecidos são ignorados.
- Espaços no início e no fim das strings são removidos.
- `Content-Type` não precisa ser exatamente `application/json`.
- Tamanho máximo do corpo: 1024 bytes.
- `cliente` e `estado` não podem conter `/`, `\` ou `..` (retorna `400`).

### Resolução do preset

O arquivo sempre se chama `exemplo.conf`. O Prog tenta nesta ordem e usa o primeiro que existir:

| Ordem | URL (com os valores do exemplo) |
|-------|---------------------------------|
| 1 | `https://preset.ppa.com.br/agility123/008249/exemplo.conf` (cliente) |
| 2 | `https://preset.ppa.com.br/agility123/SP/exemplo.conf` (estado) |
| 3 | `https://preset.ppa.com.br/agility123/exemplo.conf` (padrão) |

Qualquer falha em uma URL (404, timeout, 5xx, TLS) leva à próxima. O conteúdo do arquivo é texto hexadecimal (maiúsculo ou minúsculo, espaços e quebras de linha são ignorados, sem prefixo `0x`), com no máximo 6144 caracteres.

### Exemplo no Postman

1. Crie uma nova requisição.
2. Método: `POST`.
3. URL: `http://192.168.1.50/iniciar` (troque pelo IP do Prog).
4. Aba **Headers**, adicione:

| Key | Value |
|-----|-------|
| `X-API-Key` | `abcdef12345XYZ` |
| `Content-Type` | `application/json` | (opcional)

5. Aba **Body**: marque **raw** e escolha **JSON** no seletor à direita. Cole:

```json
{
  "id": 1234,
  "produto": "F02303100",
  "cliente": "008249",
  "estado": "SP",
  "caminho": "https://download.citrox.com.br.s3.us-east-1.amazonaws.com/fw/"
}
```

6. Clique em **Send**. Esperado: `200 OK` com `{"id":1234,"status":1,"message":[]}`.

### Respostas

O corpo de todas as respostas (exceto `401`) tem o mesmo formato do `/status`: `{"id", "status", "message"}`.

| HTTP | Situação | Corpo |
|------|----------|-------|
| **200** | Preset encontrado e teste iniciado | `{"id":1234,"status":1,"message":[]}` |
| 400 | JSON inválido, `id` ou `caminho` ausentes, etc. | `status: 3` e a causa em `message` |
| 401 | `X-API-Key` ausente ou errada | vazio |
| 404 | `exemplo.conf` não existe em nenhuma das 3 URLs | `status: 3`, uma mensagem por URL tentada |
| 409 | Já existe um teste em andamento | estado do teste atual (o estado dele não é alterado) |
| 409 | Atualização de firmware, formatação ou reboot em andamento | `status: 3`, `"firmware update in progress, try again later"` |
| 500 | Falha de download (timeout, 5xx), hex inválido ou erro interno | `status: 3` e a causa em `message` |

Só o código **200** indica que o teste começou.

Mensagens de `400` (campo `message`):

| Mensagem | Causa |
|----------|-------|
| `empty, unreadable or too large body` | Corpo vazio, ilegível ou maior que 1024 bytes |
| `invalid JSON` | Corpo não é um objeto JSON válido |
| `missing id` | Campo `id` ausente ou de tipo inválido |
| `id must be a number or numeric string` | `id` é uma string não numérica |
| `missing caminho` | `caminho` ausente ou vazio |
| `caminho must start with http:// or https://` | Protocolo inválido (SMB/FTP não são suportados) |
| `cliente/estado contain invalid characters` | `cliente` ou `estado` com `/`, `\` ou `..` |
| `caminho too long` | `caminho` grande demais |

Exemplos de resposta:

```json
// 200 - iniciado
{"id":1234,"status":1,"message":[]}

// 400 - sem caminho
{"id":1234,"status":3,"message":["missing caminho"]}

// 404 - nenhum arquivo encontrado
{"id":1234,"status":3,"message":[
  "https://preset.ppa.com.br/agility123/008249/exemplo.conf: not found (404)",
  "https://preset.ppa.com.br/agility123/SP/exemplo.conf: not found (404)",
  "https://preset.ppa.com.br/agility123/exemplo.conf: not found (404)"]}

// 409 - já em execução
{"id":1233,"status":1,"message":[]}

// 500 - hex inválido
{"id":1234,"status":3,"message":["preset file is not valid hex"]}
```

Atenção: no `409` por teste em andamento, o `id` devolvido é o do teste que **já está rodando**, não o da requisição rejeitada. O campo `message` guarda no máximo 4 itens.

---

## 2. `GET /status`

Retorna o estado da execução atual (ou da última). Não tem corpo na requisição.

### Exemplo no Postman

1. Crie uma nova requisição.
2. Método: `GET`.
3. URL: `http://192.168.1.50/status` (troque pelo IP do Prog).
4. Aba **Headers**, adicione:

| Key | Value |
|-----|-------|
| `X-API-Key` | `abcdef12345XYZ` |

5. Não envie corpo (Body: **none**).
6. Clique em **Send**. Esperado: `200 OK` com `{"id":1234,"status":1,"message":[]}` (ou `status` 2 depois de ~5 s).

### Resposta (200)

```json
{"id":1234,"status":1,"message":[]}
```

| Campo | Tipo | Descrição |
|-------|------|-----------|
| `id` | número (ou string, se o `POST` enviou string) | O `id` enviado no `POST /iniciar`. |
| `status` | número | Veja a tabela abaixo. |
| `message` | array de strings | Descrição de erros. Sempre array. Vazio quando não há erro. |

| `status` | Significado | O que fazer |
|----------|-------------|-------------|
| 0 | Ocioso (nenhum teste desde o boot) | Resposta: `{"id":0,"status":0,"message":[]}` |
| 1 | Em execução | Continuar consultando |
| 2 | Finalizado com sucesso | Parar de consultar |
| 3 | Finalizado com ERRO | Parar de consultar e ler `message` |

Comportamento:
- Na fase atual o teste é simulado: `status` fica `1` por cerca de 5 s após o início e depois vira `2`.
- O resultado final permanece até o próximo `POST /iniciar` aceito ou até reiniciar o equipamento.
- Um `POST` rejeitado (`400`, `401`, `409`) **não** altera o estado exibido no `/status`.
- O `/status` responde na hora, mesmo enquanto um `POST /iniciar` ainda está baixando o preset (nesse caso retorna `status: 1`).
- Sem `X-API-Key` válida: `401` com corpo vazio.
- O valor `0` é uma adição ao combinado original (1/2/3) e precisa ser tratado pelo Stinger.

### Exemplos de resposta

```json
// Antes de qualquer teste
{"id":0,"status":0,"message":[]}

// Rodando
{"id":1234,"status":1,"message":[]}

// Concluído
{"id":1234,"status":2,"message":[]}

// Erro
{"id":1234,"status":3,"message":["preset file is not valid hex"]}
```

### Consulta repetida no Postman

Clique em **Send** a cada poucos segundos e observe o campo `status`. Pare quando for `2` ou `3`. 

---

## Fluxo recomendado

1. `POST /iniciar` com timeout acima de 30 s.
2. Se vier `200`, consultar `GET /status` a cada 2 a 5 s.
3. Parar quando `status` for `2` (sucesso) ou `3` (erro, ler `message`).
4. Se o `POST` vier `409`, aguardar o fim do teste atual (`status` 2 ou 3) e tentar de novo.
5. Se vier `400`, `404` ou `500`, corrigir a causa indicada em `message` antes de repetir.

## Testar sem o servidor de presets real

Hospede um `exemplo.conf` com, por exemplo, `00112233AABBCCDD` em um servidor local (`python -m http.server 8000` na pasta do arquivo) e use `"caminho": "http://{IP_DO_PC}:8000/"`.

## Pontos em aberto (lado Stinger)

- A função HTTP do Stinger envia `Content-Type: application/json`?
- O cliente consegue ler o corpo de respostas diferentes de 200?
- Intervalo de consulta (X segundos) e timeout da requisição?
- O campo `id` será sempre numérico ou às vezes uma string com zeros à esquerda?
- Confirmar o tratamento do `status: 0`.
