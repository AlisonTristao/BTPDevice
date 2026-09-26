# Plano de implementação da BTPDevice

**Criado em:** 2026-09-26
**Estado:** Fase 0 (levantamento)

Este é o documento vivo do projeto: decisões, arquitetura e tarefas. Quem conclui
uma tarefa marca o checkbox no mesmo commit (ver [CONTRIBUTING](../CONTRIBUTING.md#6-commits-versões-e-plano)).
Uma decisão nova ou revista entra na [tabela de decisões](#2-decisões).

---

## 1. Objetivo

Fazer com que **qualquer dispositivo IoT com ESP32** fale com o TraceView
(telemetria, terminal, comandos, criptografia e descoberta por nome) escrevendo
apenas:

1. um objeto `BtpDevice` com nome e link;
2. os tópicos que ele publica;
3. os comandos que ele aceita;
4. onde ele guarda os arquivos.

Exemplos do público-alvo: uma estação meteorológica, um datalogger, um atuador
(relé, válvula), uma bancada de testes, um robô. Todos têm os mesmos problemas de
comunicação, e nenhum deveria resolvê-los de novo.

### Não-objetivos da v1

- Outros microcontroladores ou frameworks (Arduino, Zephyr, STM32).
- Mais de um link ativo ao mesmo tempo (ver [D4](#2-decisões)).
- Papel de hub ou roteador (o que o bally_dongle faz).
- Drivers de sensores, atuadores ou LED/buzzer. O hardware é do usuário.
- OTA, máquina de estados da aplicação e agendador de tarefas. São do produto.

### De onde vem o código

O bally_OS (firmware do Bally Robot) já tem tudo isso funcionando com o TraceView
por Serial, TCP, BLE e ESP-NOW, mas acoplado à classe `ROBOT`. A BTPDevice parte
desse código, **generalizando o que é específico do robô**. O bally_OS é a
implementação de referência e o primeiro usuário real, mas não é o centro da lib.
A [seção 5](#5-origem-do-código) mapeia o que vem de onde.

---

## 2. Decisões

| # | Decisão | Motivo |
|---|---|---|
| D1 | Nome: repositório **BTPDevice**, classe **`BtpDevice`**, namespace **`btpdev`** | — |
| D2 | **Só ESP-IDF**, como componente (6.0+, referência 6.0.1) | É o que o bally_OS usa. O BTP precisa do backend PSA/mbedtls do IDF para cifrar. |
| D3 | **Só família ESP32.** ESP32-S3 primeiro, depois ESP32, C3 e C6 | Não abraçar o mundo. |
| D4 | **Um link por boot** na v1. Internamente é um `btp::Node` com um link (o link 0) | Simples de validar. O `attach_link` do BTP 2.47 fica pronto para uma v2 multi-link sem quebrar a API. |
| D5 | Arquivos em **LittleFS** por padrão. SD como opção. **NVS** para valores pequenos (chave, link escolhido) | O LittleFS é mais robusto a queda de energia que o SPIFFS, que está em modo manutenção no IDF. |
| D6 | **Quatro arquivos de configuração** do usuário: device, storage, commands, topics | A parte densa sai do `main`. |
| D7 | Erros explícitos: `ESP_LOG*` com tag por módulo, `esp_err_t` + `last_error()` e ring de erros com limite de taxa | Nenhuma falha silenciosa. Ver [4.12](#412-erros-e-logs). |
| D8 | O `btp::Node` tem **um dono só**: a task de comunicação. Os drivers só enfileiram bytes | Elimina locks e as corridas documentadas no bally_OS. |
| D9 | **Nenhuma alocação depois do `begin()`**. O orçamento vem de um preset de memória | Comportamento previsível em campo. |
| D10 | Identificadores e comentários de código em inglês. Docs e commits em português | Consistente com BTP, TinyShell e bally_OS. |
| D11 | Sinalização por LED/buzzer (`on_event`/indicador) **fica fora da v1** | Foi adiada. O estado continua consultável por `device.state()`. |
| D12 | Se o link pedido não sobe (sem chave, sem suporte no build ou no chip), a lib **cai para Serial** automaticamente | Sem Serial não há como corrigir pelo terminal. |
| D13 | **A cifragem segue a chave, em todos os links:** com chave E carregada, sela e exige selado; sem chave, texto claro | Revisado na Fase 0 ([resultado](#fase-0-resultados)): o TraceView sela tudo e recusa texto claro quando tem chave configurada para o dispositivo. Uma serial sempre em claro quebraria nesse caso. |
| D14 | Flash encryption e NVS encryption documentadas como **opcionais** | Sem elas, a chave no NVS é tão protegida quanto um arquivo. |
| D15 | BTP e TinyShell viram **componentes ESP-IDF** (bloco `if(ESP_PLATFORM)` no CMake de cada um) | O usuário recebe tudo pelo component manager. O pacote com git + tag é validado na Fase 7. Até lá, o desenvolvimento usa as pastas vizinhas (`../BTP`, `../TinyShell`). |
| D16 | O `PeerCache` fica **para depois da v1**, com o desenho fixado em [4.13](#413-peercache-futuro) | O produtor não precisa dele. |
| D17 | O protocolo usa os recursos do `btp::Node` diretamente: comandos assíncronos (`pending` + `complete_command`), `emit_status`, registro de publicação e catálogo | Achado da Fase 0. `CommandProcessor`, `StatusReporter`, `TelemetryPublisher` e `ManifestCatalog` do bally_OS **não são portados**. |
| D18 | Os presets de memória são **três instanciações de `btp::StaticNode`**. A escolhida é criada no `begin()` com `heap_caps_malloc` + placement new. "Bytes explícitos" sai da v1 | O `StaticNode` é dimensionado por template. Ajuste fino fica para um Kconfig depois. |

---

## 3. Como o usuário usa

### 3.1 Estrutura de um projeto

```
meu_sensor/
├── CMakeLists.txt
├── main/
│   ├── idf_component.yml     ← depende de btpdevice
│   ├── main.cpp
│   └── btp/
│       ├── btp_device.h      ← 1. nome, link, memória, rede
│       ├── btp_storage.h     ← 2. LittleFS/SD, chave
│       ├── btp_commands.cpp  ← 3. comandos do terminal
│       └── btp_topics.cpp    ← 4. tópicos de telemetria
└── partitions.csv            ← partição "storage" para o LittleFS
```

### 3.2 `main.cpp`

```cpp
#include <BtpDevice.h>
#include "btp/btp_device.h"
#include "btp/btp_storage.h"

void app_commands(btpdev::Commands& c);
void app_topics(btpdev::Topics& t);

BtpDevice device(device_config, storage_config);

extern "C" void app_main() {
    if (device.begin(app_commands, app_topics) != ESP_OK) {
        // a causa já saiu no ESP_LOGE; device.last_error() tem o detalhe
    }
    while (true) {
        read_sensors();                        // código do usuário
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
```

No modo padrão (`Threading::Auto`) o usuário não chama mais nada: a lib tem a
própria task de comunicação.

### 3.3 `btp_device.h`

```cpp
const btpdev::DeviceConfig device_config = {
    .name      = "estacao-quintal",           // BLE, mDNS, USB, manifesto
    .link      = btpdev::Link::FromSettings,  // lê do NVS; senão usa .fallback
    .fallback  = btpdev::Link::Tcp,
    .memory    = btpdev::Memory::Small,       // Small / Medium / Large / bytes
    .threading = btpdev::Threading::Auto,
    .comms     = { .core = 0, .priority = 10, .period_ms = 1 },
    .wifi      = { .file = "wifi.txt" },      // ssid,senha por linha
    .tcp       = { .port = 44300 },
    .source_info = { { "hw", "Hardware", "rev B" } },
};
```

### 3.4 `btp_storage.h`

```cpp
const btpdev::StorageConfig storage_config = {
    .fs  = { .partition = "storage", .format_if_empty = true },   // LittleFS
    .key = btpdev::KeySource::Nvs,     // importa /bally.key uma vez, depois renomeia
    .sd  = std::nullopt,               // opcional: SD como segunda raiz
};
```

### 3.5 `btp_topics.cpp`

```cpp
extern float temperature, humidity, pressure;

void app_topics(btpdev::Topics& t) {
    // Ligado a variáveis: a lib lê na hora de publicar, sem código extra
    t.topic("weather", btpdev::Hz(1))
     .f32("temperature", &temperature, "°C",  -40.f,  85.f)
     .f32("humidity",    &humidity,    "%",     0.f, 100.f)
     .f32("pressure",    &pressure,    "hPa");

    // Ligado a uma função: snapshot coerente ou valor calculado
    t.topic("power", btpdev::Hz(0.2))
     .fill([](btpdev::Sample& s) {
         s.put("battery_v", read_battery());
         s.put("solar_ma",  read_solar());
     });

    // Texto livre (Text Board do TraceView)
    t.text("status", btpdev::Hz(0.5), [](btpdev::Text& out) {
        out.printf("uptime %lus", uptime_s());
    });
}
```

### 3.6 `btp_commands.cpp`

```cpp
uint8_t relay_set(uint8_t on)      { gpio_set_level(RELAY, on); return RESULT_OK; }
uint8_t sample_period(uint32_t ms) { set_period(ms); return RESULT_OK; }

void app_commands(btpdev::Commands& c) {
    c.module("relay", "Relé da bomba")
     .add("set", relay_set, "relay -set <0|1>");
    c.module("sensor", "Amostragem")
     .add("period", sample_period, "sensor -period <ms>");
}
```

### 3.7 API avançada (opcional)

| Chamada | Para quê |
|---|---|
| `device.routine()` / `device.tick()` | Modo `Threading::Manual`: o usuário controla quando a comunicação roda. `routine()` faz um passo completo (fila de entrada, comandos, tópicos, envio e `tick()`). `tick()` faz só a manutenção (watchdog de sessão, leases, STATUS). A semântica é a mesma do `btp::Node`. |
| `device.publish("weather")` | Publica agora, por evento, fora da taxa. |
| `device.print(...)` | Escreve no terminal do TraceView. |
| `device.state()`, `device.degraded()` | Estado do ciclo de vida ([4.5](#45-ciclo-de-vida)). |
| `device.set_link(Link::Ble)` | Grava no NVS e reinicia no novo link. |
| `device.stats()`, `device.last_error()` | Contadores e último erro. |
| `device.node()`, `device.shell()`, `device.fs()` | Acesso direto ao `btp::Node`, ao `TinyShell` e ao armazenamento. |

---

## 4. Arquitetura

### 4.1 Camadas

```
┌──────────────────────── aplicação do usuário ────────────────────────┐
│  main.cpp · btp_device.h · btp_storage.h · btp_commands · btp_topics │
└───────────────────────────────┬──────────────────────────────────────┘
                     BtpDevice (include/BtpDevice.h)
┌───────────────────────────────┴──────────────────────────────────────┐
│ core/      ciclo de vida · task comms · task shell · fila RX · erros │
├──────────────┬────────────────┬──────────────────┬───────────────────┤
│ protocol/    │ transport/     │ security/        │ storage/          │
│ Commands     │ SerialLink     │ KeyStore         │ LittleFs          │
│ Terminal     │ TcpLink+WiFi   │ Seal (E / L)     │ SdCard            │
│ Topics       │ BleLink        │                  │ Nvs               │
│ Status       │ EspNowLink     │                  │                   │
│ Catalog      │ Cobs           │                  │                   │
├──────────────┴────────────────┴──────────────────┴───────────────────┤
│               BTP (btp::Node) · TinyShell · ESP-IDF                  │
└──────────────────────────────────────────────────────────────────────┘
```

### 4.2 Matriz de suporte

| Link | Como | ESP32-S3 | ESP32 | ESP32-C3 | ESP32-C6 |
|---|---|---|---|---|---|
| Serial | TinyUSB CDC | ✅ v1 | — | — | — |
| Serial | USB-Serial-JTAG | depois | — | depois | depois |
| Serial | UART (conversor USB) | depois | depois | depois | depois |
| TCP | Wi-Fi STA + mDNS | ✅ v1 | depois | depois | depois |
| BLE | NimBLE GATT | ✅ v1 | depois | depois | depois |
| ESP-NOW | via bally_dongle | ✅ v1 | depois | depois | depois |

O link Serial tem três backends atrás da mesma interface, escolhidos pelo chip e
pela configuração. O TraceView trata os três igual (porta serial com COBS).

### 4.3 Os links

Os quatro links têm a mesma forma: `start / stop / send(frame, prioridade) /
has_client`, mais os callbacks `on_receive / on_connect / on_disconnect`. **Nenhum
deles conhece o BTP.** O enquadramento é do `core`: Serial, TCP e BLE usam
`0x00 || COBS(frame) || 0x00`, e o ESP-NOW usa um datagrama por frame.

- **Serial:** a conexão acontece quando o host levanta o DTR (contrato do
  `SerialTransport` do TraceView). Os bytes que chegam com o DTR baixo também
  abrem a conexão.
- **TCP:** servidor na porta 44300, um cliente ativo por vez e segunda conexão
  recusada com `HELLO_RESULT BUSY`. A lib é **dona do Wi-Fi STA**: redes vêm de
  `wifi.txt` (ou da config), há reconexão com backoff e o hostname mDNS é derivado
  do nome.
- **BLE:** serviço GATT do BTP. O anúncio é interrompido enquanto há uma central
  conectada.
- **ESP-NOW:** para dispositivos que falam com o TraceView através do dongle. Usa
  a fila com prioridades (`TxScheduler`) e as duas chaves do contrato de canais
  ([4.9](#49-segurança-e-chave)).

Todos os links usam a mesma admissão por prioridade: o topo da fila é reservado
para respostas, então uma rajada de telemetria nunca impede uma resposta de sair.

### 4.4 Threads

| Task | Dono | O que faz |
|---|---|---|
| Driver do link (TCP select, NimBLE host, TinyUSB, UART, callbacks Wi-Fi) | transport | Só copia bytes e desenquadra COBS para a **fila RX** |
| **`btpdev_comms`** | core | **Dona exclusiva do `btp::Node`**: fila RX → `receive`, publicação dos tópicos assinados, drenagem da TX, `tick()` e esvaziamento do ring de erros |
| **`btpdev_shell`** | core | Executa comandos do TinyShell, para que um comando lento não trave a comunicação. Responde pela fila de conclusão do Node (`complete_command`). |

- Com `Threading::Manual`, a `btpdev_comms` não é criada e o usuário chama
  `routine()`.
- Core, prioridade e stack são configuráveis em `.comms`. O padrão é core 0, junto
  com os rádios.

### 4.5 Ciclo de vida

```
Stopped → Starting → Ready ⇄ Connected
               └───→ Failed          (begin() retornou erro)

Sub-estados do link: Off · WifiConnecting · Retrying · Listening/Advertising/WaitingHost
Bits de degradação:  NoStorage · NoKey · LinkFallback
```

O `begin()` segue sempre esta ordem:

1. Validar a configuração (tópicos, comandos, combinação de link e chip).
2. Alocar o orçamento de memória.
3. Montar o armazenamento.
4. Carregar ou importar a chave.
5. Montar a identidade e o catálogo.
6. Subir o link.
7. Criar as tasks.

Cada passo que falha tem uma consequência definida ([4.12](#412-erros-e-logs)).
Cada transição sai em `ESP_LOGI`.

### 4.6 Identidade e descoberta

- **`source_id`** vem do MAC (4 bytes baixos). O **`boot_id`** muda a cada boot.
- **O nome é único e alimenta tudo:** o anúncio BLE (até 29 bytes), o hostname
  mDNS (sanitizado para `a-z0-9-`), o product string da USB, o `source_info.name`
  do manifesto e o nome visto pelo dongle.
- O TraceView já conecta por nome ("Automático") em Serial, TCP, BLE e Hub. A lib
  só precisa publicar o nome de forma consistente.

### 4.7 Telemetria

- **Builder de tópicos** ([3.5](#35-btp_topicscpp)). Os IDs são automáticos:
  `topic_id` pela ordem de declaração a partir de uma base, e `field_id`/`order`
  pela posição. Escala 1, offset 0 e sem faixa são os padrões.
- Tipos: `f32`, `f64`, `i8…i64`, `u8…u64`, `bool`, arrays e tópico de texto UTF-8
  (sem campos).
- **Só publica com assinatura:** usa o registro de publicação do `btp::Node`
  (`on_publish` / `publish_subscribed_topics`), na taxa concedida. A política de
  taxa por tópico (`max` / `min` / `default`) vem do builder.
- **Revisão automática do manifesto** (BTP 2.48): mudou um tópico, mudou a
  revisão. O TraceView usa o cache dele quando a revisão bate e recebe
  NOT_MODIFIED.
- Validação no `begin()`: nome de tópico ou campo repetido, tipo inválido e pool
  do catálogo pequeno demais viram erro com o nome do tópico. Quando der, isso é
  checado em tempo de compilação.
- Ler um `float` alinhado ligado por ponteiro é atômico no ESP32. Campos que
  precisam ser coerentes entre si devem usar `.fill()`.

### 4.8 Comandos e terminal

- O TinyShell fica por baixo. **O mesmo registro atende o terminal do TraceView
  (TERMINAL_IN) e o COMMAND_REQUEST.**
- Módulo embutido **`btp`** (um só, para não colidir com os módulos do usuário):
  `btp -status`, `-link`, `-errors`, `-key`, `-wifi`, `-fs`, `-reboot`. Cada
  subcomando pode ser desligado.
- A execução é assíncrona: o comando entra na fila da `btpdev_shell` e a resposta
  sai pelo link por onde o pedido chegou, com proteção contra resposta para uma
  conexão que já caiu.
- A saída do shell vai para o terminal do TraceView com as cores do TinyShell.

### 4.9 Segurança e chave

- O **contrato de canais** (`bally_channels.h`) é do ecossistema, não de um
  produto: TraceView ↔ dispositivo usa a **chave E** (uma por dispositivo), e
  dongle ↔ dispositivo usa a **chave L** (uma por frota). A BTPDevice carrega uma
  das três cópias idênticas, junto com o teste de hash.
- Por link: Serial, TCP e BLE usam chave E. ESP-NOW usa E ou L conforme o peer.
  **A cifragem segue a chave** ([D13](#2-decisões)): com chave, sela e exige
  selado; sem chave, texto claro. É o mesmo comportamento do TraceView, que sela
  quando tem a chave do dispositivo.
- **Armazenamento da chave:** fica no NVS. Na primeira vez, o usuário coloca o
  `bally.key` (96 bytes, formato do `provision_key.py`) no LittleFS ou no SD. A lib
  valida (formato, CRC, `verify_e`/`verify_l`), importa para o NVS e renomeia o
  arquivo para `bally.key.imported`. Também dá para importar pelo terminal com
  `btp -key import`.
- **Sem chave:** TCP, BLE e ESP-NOW não sobem e a lib cai para Serial em texto
  claro ([D12](#2-decisões)), com um `ESP_LOGE` explicando como provisionar.
- O `provision_key.py` passa a morar neste repositório, em `tools/`.

### 4.10 Armazenamento

| Onde | O que guarda |
|---|---|
| NVS | Chave, link escolhido (`Link::FromSettings`), contadores persistentes da lib |
| LittleFS (partição `storage`) | `wifi.txt`, arquivo de importação da chave, cache (futuro) e arquivos do usuário via `device.fs()` |
| SD (opcional) | Segunda raiz para os mesmos arquivos. O usuário escolhe qual raiz a lib usa. |

- A interface `btpdev::FileSystem` é pequena: `read`, `write`, `append`, `exists`,
  `remove`, `rename`, `list`, `info`. LittleFS e SD implementam essa interface.
- Se o LittleFS não montar e `format_if_empty` estiver ligado, a lib formata na
  primeira vez. Se ainda assim falhar, o dispositivo segue **sem persistência**,
  com o bit `NoStorage` ligado e `ESP_LOGE`.

### 4.11 Memória

Cada preset é uma instanciação de `btp::StaticNode` ([D18](#2-decisões)) mais as
filas dos links. Os tamanhos abaixo são o ponto de partida, medidos na Fase 0 no
host (64 bits; no ESP32 os ponteiros têm metade do tamanho).

| Preset | Uso típico | Tópicos / campos | Assinaturas | Node (host) |
|---|---|---|---|---|
| `Small` | Sensor com poucos tópicos | 4 / 16 | 4 | ~10 KB |
| `Medium` | Padrão | 8 / 48 | 8 | ~19 KB |
| `Large` | Muitos tópicos, manifesto grande | 16 / 96 | 16 | ~34 KB |

O Node escolhido é alocado no `begin()`, na PSRAM quando houver. Se não couber, o
`begin()` falha e diz quanto pediu e quanto havia. Os números são revistos na
Fase 3 com medições no alvo.

### 4.12 Erros e logs

**Tags:** `btpdev`, `btpdev.serial`, `btpdev.tcp`, `btpdev.wifi`, `btpdev.ble`,
`btpdev.espnow`, `btpdev.key`, `btpdev.fs`, `btpdev.cmd`, `btpdev.topic`. O nível
padrão vem de um Kconfig (`BTPDEV_LOG_LEVEL`) e pode ser ajustado por tag com
`esp_log_level_set`.

**Três camadas:**

1. **Boot e configuração:** `begin()` retorna um `esp_err_t` com códigos próprios
   (`BTPDEV_ERR_NO_KEY`, `_LINK_UNSUPPORTED`, `_MEMORY_BUDGET`, `_BAD_TOPIC`,
   `_STORAGE`, …) e `btpdev_err_name()`. `last_error()` devolve
   `{ código, módulo, mensagem }`.
2. **Execução:** os callbacks de driver só incrementam contadores ou empurram para
   um ring fixo sem lock. A `btpdev_comms` esvazia o ring e loga com limite de
   taxa: a primeira ocorrência completa, depois um resumo por segundo.
3. **Remoto:** os contadores vão no STATUS do BTP (o TraceView já mostra), e
   `btp -errors` lista os últimos N erros no terminal.

**Política de degradação:**

| Falha | Consequência | Log |
|---|---|---|
| Configuração inválida (tópico ou comando) | `begin()` falha | E |
| Orçamento de memória insuficiente | `begin()` falha, dizendo quanto pediu e quanto havia | E |
| LittleFS não monta | Formata se permitido. Senão segue sem persistência (`NoStorage`) | E |
| Sem chave, ou chave inválida | O link que exige chave não sobe e a lib cai para Serial (`NoKey`, `LinkFallback`) | E |
| Link não compilado ou não suportado pelo chip | Cai para Serial (`LinkFallback`) | E |
| Wi-Fi não conecta | Tenta de novo com backoff, sem trocar de link | W por tentativa, E após N |
| Fila cheia ou frame inválido | Contador mais resumo por segundo | W |

**Nunca** aparece chave em log. Para diagnóstico, só `verify_e`/`verify_l`.

### 4.13 PeerCache (futuro)

Serve para quando um dispositivo **consome** manifestos de outros (um hub, ou um
ESP32 que lê outro pelo ESP-NOW). O produtor já se beneficia hoje sem cache
próprio: responde NOT_MODIFIED e o TraceView guarda o cache.

- **Chave:** `source_id` é a identidade. O nome é só um índice de busca, e um nome
  ambíguo não resolve (mesma regra do TraceView).
- **Cada entrada:** nome, `source_uuid`, `config_revision`, último `boot_id`,
  visto por último em, endereço do link e o payload bruto do MANIFEST_DATA.
- **Onde:** `/btpdev/peers/index.bin` e `/btpdev/peers/<source_id>.man` no
  LittleFS. Em RAM fica só o índice.
- Implementa os hooks `manifest_load / manifest_store / manifest_evict` do
  `NodeConfig` (BTP 2.48).

---

## 5. Origem do código

O que vem do bally_OS (`c:/git/bally_OS`, branch `main`) e o que muda para ficar
genérico:

| Destino | Origem no bally_OS | Generalização necessária |
|---|---|---|
| `transport/TcpLink` | `lib/TcpBtpServer`, `lib/BtpTransport/TcpSendAdmission.h` | Pouca: já é independente do BTP |
| `transport/BleLink` | `lib/BleBtpServer` | Pouca |
| `transport/SerialLink` | `lib/UsbSerialBtpServer` | Separar o backend TinyUSB, instalar o driver USB sozinho (hoje quem instala é o `USBMassStorage`) e criar os backends USB-Serial-JTAG e UART |
| `transport/Wifi` | Modo direto do `lib/OTAUpdater` (STA, `wifi.txt`, mDNS) | Extrair só a parte de rede. O OTA continua no produto. |
| `transport/EspNowLink` | `ROBOT::handleReceiveStatic` / `handleSendStatic`, `lib/TxScheduler` | Tirar do `ROBOT` |
| `core/Cobs` | `CobsStreamDecoder` / `sendCobsFramed()` (`lib/BtpTransport`) | — |
| `protocol/Commands` | `runShell()` e o formato do pedido em `btp_command` (`lib/BtpTransport`): ação 1 = linha de shell, ação 2 = ping | **Não porta o `CommandProcessor`** ([D17](#2-decisões)): usa `enable_commands` + `pending` + `complete_command` do Node |
| `protocol/Terminal` | `lib/TerminalResponder` | Um alvo (o link ativo) em vez de listas por transporte |
| `protocol/Topics` | Nada é portado: só o modelo de `TopicSchema` serve de referência | **API nova** (builder) sobre `Catalog` + `on_publish` do Node ([D17](#2-decisões)) |
| `protocol/Status` | Nada é portado | `Node::emit_status` ([D17](#2-decisões)) |
| `security/KeyStore` | `lib/KeyStore` | Ler de bytes ou do NVS, sem depender do `SDCard` |
| `security/Seal` | `lib/RadioSeal` | — |
| `storage/SdCard` | `lib/SDCard` | Implementar `btpdev::FileSystem` |
| `include/btpdev/bally_channels.h` | `include/bally_channels.h` + `scripts/check_channel_contract.py` | Nenhuma: cópia byte a byte |
| `tools/provision_key.py` | `scripts/provision_key.py` (+ teste) | — |
| Composição (`core/`) | `BallyRobot.cpp` ~956–1618 (`bindProtocolTransport`, `onTcp*`, `onBle*`, `onSerial*`) | **Reescrever** para um Node, um link e uma fila RX |

**Referência extra:** a branch `feature/btp-multi-link-node` do bally_OS (worktree
`c:/git/bally_OS-multilink`, não mergeada) tem o desenho em
`docs/btp-multi-link-node.md` e um `ROBOT` com um Node de 3 links. Ela é útil
para a v2 multi-link e para ver o que o link 0 precisa.

**Fica no produto** (não entra na lib): máquina de estados, Logger com ring em
PSRAM, OTA, USB Mass Storage, jobs/autoexec, `RobotSettings`, seletor de modo por
botões e os tópicos e comandos específicos de cada dispositivo.

---

## 6. Fases

Cada fase termina com algo que roda. Os checkboxes são marcados no commit que
conclui a tarefa.

### Fase 0: levantamento (sem código de produto)

- [x] Listar exatamente o que o TraceView exige de um dispositivo por link (HELLO, manifesto, `source_info.name`, STATUS, contrato de DTR na serial, nome BLE e mDNS) e registrar aqui.
- [x] Confirmar que um `btp::StaticNode` com um link cobre tudo o que `node_`/`tcp_node_`/`ble_node_`/`serial_node_` fazem hoje: sessão, assinatura, terminal, comandos assíncronos (`pending` + `complete_command`), manifesto e NOT_MODIFIED.
- [x] Revisar a branch `feature/btp-multi-link-node` e anotar o que se aproveita.
- [x] Medir o `sizeof` dos objetos de comunicação para chegar aos presets de memória (no host). *A medição de heap no alvo fica para a Fase 3, com a lib rodando.*
- [x] Levantar como o dispositivo descobre o dongle no ESP-NOW hoje (MAC, canal) e como isso fica sem o `ROBOT`.
- [x] Fechar as decisões pendentes P1–P5 (viraram D12–D16).

**Pronto quando:** medições e decisões estão registradas neste arquivo.

#### Fase 0: resultados

**O que o TraceView exige de um dispositivo**

| Item | Regra |
|---|---|
| Enquadramento | Serial, TCP e BLE: `0x00 ‖ COBS(frame) ‖ 0x00` num fluxo de bytes. ESP-NOW: um frame por datagrama, sempre pelo dongle. |
| Perfil de frame | O TraceView ainda codifica TCP e BLE com o perfil `kSerialTransport` (4096/4056), enquanto não sobe o pin do BTP. O dispositivo precisa aceitar frames desse tamanho nos três links de fluxo. |
| Serial | Conexão = DTR alto (o TraceView liga DTR+RTS ao abrir e baixa RTS antes de DTR ao fechar). 1200 baud é o atalho do esptool para o bootloader e nunca deve ser usado. O nome do dispositivo aparece na descrição da porta USB (product string). |
| TCP | Porta padrão 44300. Busca por `<nome>.local` (mDNS). Um cliente ativo; a segunda conexão recebe `HELLO_RESULT BUSY`. |
| BLE | Serviço GATT do BTP (UUIDs em `BTP/docs/fragmentation-and-transports.md` §8), nome no scan response, comparado sem diferenciar maiúsculas e sem espaços nas pontas. |
| Sessão | HELLO/HELLO_RESULT nos links diretos. O `peer_uuid` precisa ser **estável** (o TraceView revalida a cada conexão), então é derivado do MAC. |
| Manifesto | `source_info.name` é o nome mostrado no card. A revisão automática permite NOT_MODIFIED contra o cache do TraceView (`ManifestStore`, por `source_id`). |
| Cifragem | O TraceView sela tudo e recusa texto claro quando o dispositivo tem chave configurada (`BtpBackend::accept_cleartext`). Daí veio a [D13](#2-decisões). |

**Cobertura de um Node com um link.** Cada Node do bally_OS (`node_`, `tcp_node_`,
`ble_node_`, `serial_node_`) já é um `StaticNode` de um link, então um Node com um
link cobre cada modo por construção. A diferença entre modos é só a sessão:
ligada nos links diretos e desligada no ESP-NOW. Além disso, o Node do BTP 2.48 já
faz o que no robô é código próprio: comandos assíncronos com `NodeCommandTicket` (que
guarda o `LinkRef` e não responde a uma conexão que já caiu), `emit_status`,
registro de publicação e catálogo com revisão automática. Daí a [D17](#2-decisões).
O `complete_command` pode ser chamado de qualquer task, mas pela
[D8](#2-decisões) a task de shell entrega o resultado à task de comunicação, e só
ela chama o Node.

**Branch multi-link.** O desenho (`NodeLink`, sessão e assinatura por link,
`LinkRef` com época) já está no BTP 2.47. O lado do robô só interessa para a v2
multi-link. O que se aproveita agora é o conceito de `LinkRef` para respostas
atrasadas, que o `NodeCommandTicket` já carrega.

**Memória.** `sizeof` no host (64 bits) de um `StaticNode`: o do bally_OS
(`<4,600,1900,1900,6,24,768,…>`) tem ~16 KB, o padrão tem ~19 KB, um "small" tem
~10 KB e um "large" tem ~34 KB. O robô mantém **quatro** Nodes estáticos hoje. Com
um Node por boot, a BTPDevice economiza uns 45 KB só aí.

**ESP-NOW e dongle.** Hoje o MAC do dongle vem de um flag de build (`MAC_ADDR`) e o
canal vem do settings (`espnow_channel`). O `source_id` do dongle é derivado do MAC
dele. Na BTPDevice isso vira `.espnow = { .peer_mac, .channel }` no
`btp_device.h`. O pareamento automático (aprender o MAC do primeiro frame
autenticado com a chave L e guardar no NVS) fica como melhoria depois da v1.

### Fase 1: infraestrutura do repositório

- [ ] BTP e TinyShell como componentes IDF ([D15](#2-decisões)).
- [ ] `CMakeLists.txt`, `idf_component.yml` e `Kconfig` (`BTPDEV_LOG_LEVEL`, `BTPDEV_LINK_*`) da BTPDevice.
- [ ] Esqueleto de `include/BtpDevice.h` e `include/btpdev/` compilando vazio.
- [ ] `test/host/` com CMake + CTest e um teste trivial.
- [ ] `test_apps/build_smoke` compilando para ESP32-S3.
- [ ] CI no GitHub Actions: testes de host e build IDF dos exemplos e `test_apps`.

**Pronto quando:** `ctest` e o build no alvo passam na CI.

### Fase 2: transportes

- [ ] Trazer o COBS, com os testes.
- [ ] `SerialLink` (backend TinyUSB CDC), com `test_apps/serial_echo`.
- [ ] `Wifi` (STA, `wifi.txt`, backoff, mDNS) e `TcpLink`, com `test_apps/tcp_echo`.
- [ ] `BleLink`, com `test_apps/ble_echo`.
- [ ] `EspNowLink` e fila com prioridades, com `test_apps/espnow_echo`.
- [ ] Admissão por prioridade comum a todos os links.

**Pronto quando:** cada `test_app` ecoa frames COBS para um script no PC (ou para o dongle, no caso do ESP-NOW).

### Fase 3: núcleo

- [ ] Tipos de configuração (`DeviceConfig`, `StorageConfig`, `Link`, `Memory`, `Threading`).
- [ ] Infraestrutura de erros: tags, códigos, `last_error()`, ring e limite de taxa.
- [ ] Ciclo de vida e ordem do `begin()` com a política de degradação.
- [ ] Orçamento de memória e alocação única.
- [ ] Identidade: `source_id` do MAC e nome propagado para BLE, mDNS, USB e `source_info`.
- [ ] Fila RX única, `btpdev_comms`, `routine()`/`tick()` e `Threading::Manual`.
- [ ] Contrato de canais (`bally_channels.h`) e `Seal`; `KeyStore` carregando de bytes.
- [ ] **Exemplo `01_minimal`**: Serial, um tópico e o terminal do TraceView.

**Pronto quando:** o TraceView encontra o `01_minimal` por nome na serial, abre o terminal e plota o tópico.

### Fase 4: telemetria e comandos

- [ ] Builder de tópicos (ponteiro, `.fill()`, texto) com IDs automáticos e validação.
- [ ] Integração com o registro de publicação do Node e com a revisão automática.
- [ ] `Commands` sobre o TinyShell, `btpdev_shell` e respostas assíncronas pelo link de origem.
- [ ] Módulo embutido `btp` (`-status`, `-link`, `-errors`, `-wifi`, `-fs`, `-reboot`).
- [ ] **Exemplo `02_weather_station`** (TCP, tópicos ligados a variáveis).
- [ ] **Exemplo `03_relay_actuator`** (comandos e confirmação).

**Pronto quando:** os três exemplos funcionam com o TraceView por Serial, TCP e BLE, trocando só o `.link`.

### Fase 5: segurança e armazenamento

- [ ] `btpdev::FileSystem` com LittleFS (montar, formatar na primeira vez).
- [ ] Backend SD (SPI e SDMMC).
- [ ] Chave no NVS: importar do arquivo, `btp -key import/status`, sem nunca logar a chave.
- [ ] `Link::FromSettings` e `set_link()` no NVS.
- [ ] `tools/provision_key.py` e o teste dele.
- [ ] **Exemplo `04_ble_secure`** (BLE com chave E, TraceView no Android).
- [ ] **Exemplo `05_espnow_dongle`** (ESP-NOW através do bally_dongle).

**Pronto quando:** um dispositivo novo sai do zero para cifrado e conectado seguindo só o README do exemplo.

### Fase 6: primeiro produto real (bally_OS)

Validar a API numa aplicação grande. É o teste de fogo da lib, não o objetivo dela.

- [ ] Branch/worktree no bally_OS usando a BTPDevice como componente.
- [ ] Os tópicos e comandos do robô viram os arquivos de configuração dele.
- [ ] Resolver a divisão do Wi-Fi com o OTA do robô e do USB com o Mass Storage.
- [ ] Remover do bally_OS as libs que passaram para a BTPDevice.
- [ ] Teste no hardware: TraceView por Serial, TCP e BLE, e dongle por ESP-NOW.

**Pronto quando:** o robô faz tudo o que fazia antes, com a comunicação inteira vindo da BTPDevice.

### Fase 7: v1.0.0

- [ ] README de cada exemplo e guia "meu primeiro dispositivo".
- [ ] Referência da API pública.
- [ ] Tag `v1.0.0`.

### Depois da v1

- Chips ESP32, C3 e C6, e backends Serial USB-Serial-JTAG e UART.
- `PeerCache` ([4.13](#413-peercache-futuro)).
- Vários links ao mesmo tempo (`attach_link`).
- Sinalização por evento (`on_event`, indicador de LED/buzzer).
- Parâmetros persistentes declarados como os tópicos (editáveis pelo TraceView).
- Deep sleep para sensores a bateria.

---

## 7. Riscos

| Risco | Mitigação |
|---|---|
| A lib ficar com a cara do robô | Os exemplos IoT (Fases 3–5) vêm **antes** da migração do bally_OS, e a regra do CONTRIBUTING proíbe hardware específico. |
| Wi-Fi com dois donos (lib × OTA do produto) | A lib é dona do STA e expõe o estado. O produto pede para usar o Wi-Fi, não inicializa o próprio. Será validado na Fase 6. |
| USB composto (CDC da lib + MSC do produto) | Opção para o produto instalar o driver USB e a lib só iniciar o CDC. |
| RAM do NimBLE (~50–70 KB) | O preset e a validação no `begin()` explicam o custo. O BLE pode ser desligado no Kconfig. |
| Fila RX única mudar a latência em relação ao caminho atual do ESP-NOW | Medir o tempo de ida e volta na Fase 2 e comparar com o bally_OS. |
| Versões de BTP/TinyShell divergindo entre repositórios | Tags fixas, e a subida de tag é um commit próprio com nota no CHANGELOG. |
| API congelada cedo demais | Versões `0.x` até a Fase 6 validar a API numa aplicação grande. |
