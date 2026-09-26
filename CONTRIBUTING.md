# Como contribuir com a BTPDevice

Este documento define como o código da BTPDevice é organizado e quais regras toda
mudança precisa seguir. As decisões de arquitetura e o andamento ficam em
[docs/PLANO.md](docs/PLANO.md). Leia o plano antes de propor algo grande.

## 1. O princípio: a vida de quem usa vem primeiro

A BTPDevice existe para que um dispositivo IoT novo fale com o TraceView em poucas
linhas. Toda mudança é julgada primeiro por isso.

- **O caminho simples não cresce.** O `main` do exemplo mínimo tem que continuar
  cabendo numa tela. Uma opção nova entra na struct de configuração **com um padrão
  sensato**, nunca como um parâmetro obrigatório.
- **Obrigatório é só o que não tem padrão possível:** o nome do dispositivo e o link.
- **A complexidade fica nos arquivos de configuração** (comandos, tópicos,
  armazenamento, dispositivo) e nos exemplos, não na classe `BtpDevice`.
- **Funções avançadas são opt-in**: `device.node()`, `device.shell()` e similares
  dão acesso às camadas de baixo sem obrigar ninguém a conhecê-las.
- **A lib é genérica.** Nada aqui pode assumir robô, motor, botão ou qualquer
  hardware específico. Se um recurso só faz sentido para um produto, ele fica no
  firmware do produto e usa a API pública.

## 2. Organização do repositório

```
include/BtpDevice.h        única include necessária para o usuário
include/btpdev/            tipos públicos (config, commands, topics, erros)
src/core/                  BtpDevice: composição, tasks, ciclo de vida
src/transport/             links: serial, TCP, BLE, ESP-NOW (sem conhecer o BTP)
src/protocol/              comandos, terminal, telemetria, status, catálogo
src/security/              chave e seal/open
src/storage/               LittleFS, SD, NVS
examples/                  projetos ESP-IDF completos, um por caso de uso
test/host/                 testes que rodam no PC (CMake + CTest)
test_apps/                 projetos só para compilar/testar no alvo
docs/                      plano e documentação
```

### Direção das dependências

```
core  →  protocol  →  (BTP, TinyShell)
core  →  transport
core  →  security, storage
```

- `transport/` **não conhece o BTP**: recebe e envia bytes, avisa conexão e
  desconexão. O enquadramento (COBS) e o protocolo são responsabilidade de quem
  compõe.
- `protocol/` **não conhece um transporte concreto**: envia por uma função de envio
  recebida na configuração.
- `storage/` e `security/` não dependem de `transport/` nem de `protocol/`.
- Só o `core/` junta as partes. Ciclos de dependência não são aceitos, inclusive
  entre arquivos `.cpp`.
- Os headers públicos (`include/`) não incluem headers internos (`src/`) nem
  headers pesados do IDF (NimBLE, lwIP, TinyUSB). Use forward declarations.

## 3. Regras de código

### Linguagem e estilo

- C++20, sem exceções e sem RTTI no código da lib (compila com
  `CONFIG_COMPILER_CXX_EXCEPTIONS` desligado).
- `namespace btpdev` para tudo, exceto a classe de fachada `BtpDevice`.
- Classes, arquivos e pastas em `PascalCase`. Métodos, funções e variáveis em
  `snake_case`, como no BTP. Membros privados terminam com `_`. Constantes são
  `kNomeDaConstante`.
- `noexcept` em tudo que roda em callback de driver ou na task de comunicação.
- **Idioma:** identificadores e comentários de código em **inglês**, como no BTP e
  no TinyShell. Documentação, commits e discussões em **português**.

### Memória

- **Nenhuma alocação depois do `begin()`.** Tudo que depende do orçamento de
  memória é alocado uma vez no `begin()`, com PSRAM preferida para buffers
  grandes. Se não couber, o `begin()` falha com uma mensagem que diz quanto foi
  pedido e quanto havia.
- Todo tamanho fixo (`kSendQueueDepth`, `kMaxPayload`…) tem um comentário
  dizendo de onde vem o número.
- Nada de `std::string`/`std::vector` crescendo em caminho quente. Na fase de
  registro (antes do `begin()`) é permitido.

### Concorrência

- **Um dono por estado.** O `btp::Node` só é tocado pela task de comunicação. Os
  callbacks de driver (Wi-Fi, NimBLE, TinyUSB, UART, ESP-NOW) só copiam bytes para
  a fila de entrada e retornam.
- Todo método público documenta **de qual contexto pode ser chamado** (qualquer
  task, só a task de comunicação, antes do `begin()`…).
- Callbacks de driver e ISRs **nunca** chamam `ESP_LOG*`, `printf`, funções que
  bloqueiam ou o `btp::Node`.
- Se um lock for inevitável, ele fica dentro do componente dono do estado e o
  comentário explica o que ele protege.

### Erros e logs

- Cada módulo tem sua tag: `btpdev`, `btpdev.serial`, `btpdev.tcp`, `btpdev.ble`,
  `btpdev.espnow`, `btpdev.wifi`, `btpdev.key`, `btpdev.fs`, `btpdev.cmd` e
  `btpdev.topic`.
- Níveis:

  | Nível | Quando usar |
  |---|---|
  | `E` | O usuário precisa agir, ou uma função ficou fora do ar |
  | `W` | Degradado mas funcionando: nova tentativa, fila cheia, frame descartado |
  | `I` | Ciclo de vida: link no ar, sessão aberta, chave importada |
  | `D`/`V` | Por frame ou por comando |

- **Mensagens `E` dizem o que aconteceu, a consequência e como resolver.** Por
  exemplo: `bally.key rejeitado (verify_e): senha E errada. Gere de novo com
  provision_key.py. Link TCP desativado.`
- Erros em tempo de execução passam pelo ring de erros e saem **com limite de
  taxa**: a primeira ocorrência completa, depois um resumo por segundo.
- Funções que podem falhar retornam `esp_err_t` (ou `bool` quando o motivo não
  importa) e registram o detalhe em `last_error()`. Falhas nunca são silenciosas.

### Segurança

- **Chaves nunca aparecem em log**, nem parciais, nem atrás de flag de debug. Para
  diagnóstico, use os `verify_e`/`verify_l` (imagens públicas da chave).
- Envio é **fail-closed**: se o seal falhar, nada é transmitido. Nunca existe
  fallback para texto claro num link que exige chave.
- Arquivos `*.key` nunca entram no repositório (o `.gitignore` já bloqueia).

## 4. Dependências

- BTP e TinyShell são consumidos **fixados em tag**, nunca em branch. Subir a tag
  é um commit próprio, com uma nota no CHANGELOG explicando o motivo.
- `include/btpdev/bally_channels.h` é o contrato de canais e chaves do ecossistema
  e existe em cópias **idênticas byte a byte** aqui, no bally_dongle e no
  TraceView. Para mudar: edite num repositório, copie para os outros dois e
  atualize o hash esperado nos três testes. Não existe edição parcial correta.
- Não adicione dependência nova sem discutir antes numa issue.

## 5. Build e testes

Os projetos de `test_apps/` e `examples/` esperam o BTP e o TinyShell clonados
**ao lado** deste repositório (`../BTP`, `../TinyShell`); ver
`test_apps/btpdev_components.cmake`.

```text
# Testes no PC (código portável: protocolo, chave, config, parsers)
cmake -S test/host -B test/host/build
cmake --build test/host/build
ctest --test-dir test/host/build --output-on-failure

# Compilar um projeto no alvo (ESP-IDF ou PlatformIO)
idf.py -C test_apps/build_smoke set-target esp32s3 build
pio run -d test_apps/build_smoke
```

Depois de mudar a lista de fontes ou as dependências do `CMakeLists.txt` do
componente, apague o build do PlatformIO (`.pio/build`): ele não reconfigura o
CMake sozinho e o erro aparece como `undefined reference` no link.

Código portável (sem chamadas do IDF) entra na lista `btpdev_portable` de
`test/host/CMakeLists.txt` e ganha teste. O header `btpdev/error.h` traz um
substituto do `esp_err.h` para o host, com os mesmos valores.

Antes de considerar uma mudança de código pronta:

1. Os testes de host passam.
2. Todos os exemplos compilam para os chips suportados (a CI faz isso).
3. Se a mudança toca um transporte, ela foi testada no hardware com o TraceView.
   Diga no PR qual link e qual chip foram testados.

Mudanças só de documentação precisam de revisão do diff, dos links e da coerência
com o plano. Não precisam de build.

## 6. Commits, versões e plano

- Um assunto por commit. A mensagem é em português e diz o **porquê**, não só o quê.
- Toda mudança visível para o usuário ganha uma linha em `CHANGELOG.md`, na seção
  `[Não lançado]`.
- Versionamento SemVer: mudar a API pública ou o formato de arquivo persistido
  (chave, cache) incrementa a versão major. Recurso novo compatível incrementa a
  minor. Correção incrementa a patch.
- Quem conclui uma tarefa do [plano](docs/PLANO.md) marca o checkbox no **mesmo
  commit**. Uma decisão nova ou revista entra na tabela de decisões do plano.

Ao enviar uma contribuição, você concorda que ela seja distribuída sob a
[licença MIT](LICENSE) do projeto.
