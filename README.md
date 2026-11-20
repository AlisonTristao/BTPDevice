# TraceKit

Biblioteca ESP-IDF para construir dispositivos ESP32 integrados ao
[TraceView](https://github.com/AlisonTristao/TraceView).

A TraceKit fornece a infraestrutura comum a esses dispositivos — telemetria, terminal de
comandos, criptografia, configurações persistentes, relógio, gerenciamento de energia e
atualização de firmware pela rede. A aplicação declara o que o dispositivo mede e quais
comandos aceita; a biblioteca cuida do restante. A comunicação usa o protocolo
[BTP](https://github.com/AlisonTristao/BTP) e o terminal é baseado no
[TinyShell](https://github.com/AlisonTristao/TinyShell).

> **Status:** versão 0.x, em desenvolvimento ativo. A API pode mudar entre versões
> menores. Plataforma de referência: ESP32-S3.

## Sumário

- [Recursos](#recursos)
- [Requisitos](#requisitos)
- [Instalação](#instalação)
- [Início rápido](#início-rápido)
- [Estrutura da aplicação](#estrutura-da-aplicação)
- [Configuração](#configuração)
- [Links de comunicação](#links-de-comunicação)
- [Telemetria](#telemetria)
- [Comandos e terminal](#comandos-e-terminal)
- [Configurações persistentes](#configurações-persistentes)
- [Data e hora](#data-e-hora)
- [Energia e sono profundo](#energia-e-sono-profundo)
- [Armazenamento](#armazenamento)
- [Segurança](#segurança)
- [Recovery e atualização OTA](#recovery-e-atualização-ota)
- [Comunicação entre dispositivos](#comunicação-entre-dispositivos)
- [Tasks e núcleos](#tasks-e-núcleos)
- [Memória e PSRAM](#memória-e-psram)
- [Diagnóstico](#diagnóstico)
- [Referência de comandos](#referência-de-comandos)
- [Referência da API](#referência-da-api)
- [Ferramentas](#ferramentas)
- [Exemplos](#exemplos)
- [Compatibilidade](#compatibilidade)
- [Documentação adicional](#documentação-adicional)
- [Licença](#licença)

## Recursos

| Área | Descrição |
|---|---|
| **Links** | Serial (USB nativa, USB-Serial-JTAG ou UART), TCP sobre Wi-Fi, Bluetooth LE e ESP-NOW, com fallback automático para Serial. |
| **Descoberta** | Um único nome identifica o dispositivo no anúncio BLE, no mDNS (`<nome>.local`), no hostname DHCP, no descritor USB e no manifesto. |
| **Telemetria** | Tópicos declarados uma vez e publicados apenas enquanto há assinantes, na taxa solicitada. Suporte a streams contínuos e a payloads binários. |
| **Terminal** | Comandos organizados em módulos, saída formatada e logs enviados ao TraceView, barras de progresso e comandos de sistema. |
| **Segurança** | AES-128-GCM com chave por dispositivo. Links por rádio exigem chave. |
| **Configurações** | Parâmetros da aplicação persistidos no NVS e editáveis pelo terminal ou pelo TraceView, sem recompilar. |
| **Data e hora** | Sincronização por SNTP, ajuste manual ou pelo TraceView, fuso horário configurável. |
| **Energia** | Modo de economia sem perda de conexão (modem sleep e light sleep) e sono profundo para dispositivos a bateria. |
| **Atualização** | Firmware de recovery com OTA autenticado, acionável pelo TraceView, por linha de comando ou pelo navegador. Ativação automática em caso de crash loop. |
| **Memória** | Presets de dimensionamento, uso opcional de PSRAM e relatório de heap e stack por task. |
| **Erros** | Toda falha é registrada com causa e ação sugerida; `begin()` retorna um código e uma mensagem descritiva. |

## Requisitos

- **ESP-IDF 6.0** ou superior, via `idf.py` ou PlatformIO (`framework = espidf`).
- **C++20** (a configuração usa inicializadores designados).
- Conexão com a internet no primeiro build: as dependências são baixadas pelo
  [IDF Component Manager](https://docs.espressif.com/projects/idf-component-manager/).

### Dependências

Resolvidas automaticamente a partir do [`idf_component.yml`](idf_component.yml):

| Componente | Versão | Origem |
|---|---|---|
| [BTP](https://github.com/AlisonTristao/BTP) | v2.51.0 | GitHub |
| [TinyShell](https://github.com/AlisonTristao/TinyShell) | v2.2.0 | GitHub |
| `espressif/mdns` | ^1.2 | ESP Component Registry |
| `joltwallet/littlefs` | ^1.14 | ESP Component Registry |
| `espressif/esp_tinyusb` | ^2.0.1 | ESP Component Registry (apenas S2, S3 e P4) |

## Instalação

### Como submódulo (recomendado)

Adicione a biblioteca na pasta `components/` do projeto. O ESP-IDF registra
automaticamente os componentes dessa pasta, e as dependências são baixadas no
primeiro build.

```text
git submodule add https://github.com/AlisonTristao/TraceKit.git components/TraceKit
```

O nome da pasta deve ser `TraceKit`, pois o ESP-IDF nomeia o componente a partir dela.
Esta forma de instalação funciona tanto com `idf.py` quanto com PlatformIO e dá acesso
às tabelas de partição e ao script de gravação do recovery.

### Pelo IDF Component Manager

Em projetos `idf.py` que não usam o recovery, a biblioteca também pode ser declarada
como dependência em `main/idf_component.yml`:

```yaml
dependencies:
  TraceKit:
    git: https://github.com/AlisonTristao/TraceKit.git
    version: main        # em produção, fixe uma tag
```

## Início rápido

Estrutura mínima de um projeto PlatformIO com a TraceKit em `components/TraceKit`:

```text
meu_sensor/
├── CMakeLists.txt
├── platformio.ini
├── partitions.csv          cópia de components/TraceKit/partitions/tracekit_16mb.csv
├── components/TraceKit/
├── data/                   enviado ao LittleFS (chave, wifi.txt)
└── main/
    ├── CMakeLists.txt
    └── main.cpp
```

```cmake
# CMakeLists.txt
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(meu_sensor)
```

```cmake
# main/CMakeLists.txt
idf_component_register(SRCS "main.cpp" INCLUDE_DIRS "." REQUIRES TraceKit)
```

```ini
; platformio.ini
[platformio]
src_dir = main

[env:esp32-s3]
platform = espressif32
board = esp32-s3-devkitc-1
framework = espidf
board_upload.flash_size = 16MB
board_build.partitions = partitions.csv
board_build.filesystem = littlefs
extra_scripts = post:components/TraceKit/tools/pio_recovery.py   ; grava o recovery junto com o app
```

```cpp
// main/main.cpp
#include <TraceKit.h>

float temperatura = 0.f;

class Sensor : public tracekit::App {
public:
    tracekit::DeviceConfig config() const override {
        return {.name = "meu-sensor"};
    }
    void topics(tracekit::Topics& t) override {
        t.topic("clima", tracekit::Hz(1)).f32("temperatura", &temperatura, "°C", -40.f, 85.f);
    }
};

Sensor app;
TraceKit device(app);

extern "C" void app_main() {
    device.begin();                 // retorna ESP_OK ou um código de erro (ver device.last_error())
    for (;;) {
        temperatura = le_sensor();  // a publicação é feita pela biblioteca
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
```

Para gravar:

```text
pio run -t upload        # aplicação e recovery
pio run -t uploadfs      # conteúdo de data/ para o LittleFS
```

Com a placa conectada por USB, o dispositivo aparece no TraceView como `meu-sensor`.
Sem chave provisionada, apenas o link Serial está disponível; para Wi-Fi ou BLE, consulte
[Segurança](#segurança).

## Estrutura da aplicação

A aplicação é uma classe derivada de `tracekit::App`, com o estado do dispositivo como
membros ([exemplo 03](examples/03_relay_actuator)). Apenas `config()` é obrigatório.

| Método | Finalidade |
|---|---|
| `config()` | Nome, link, memória, rede, energia e sono ([`DeviceConfig`](#deviceconfig)) |
| `storage()` | Sistema de arquivos, cartão SD e chave ([`StorageConfig`](#storageconfig)) |
| `settings(Settings&)` | Parâmetros persistentes ([Configurações persistentes](#configurações-persistentes)) |
| `commands(Commands&)` | Comandos disponíveis no terminal ([Comandos e terminal](#comandos-e-terminal)) |
| `topics(Topics&)` | Dados publicados para o TraceView ([Telemetria](#telemetria)) |

```cpp
class BombaJardim : public tracekit::App {
public:
    tracekit::DeviceConfig config() const override {
        return {.name = "bomba-jardim", .link = tracekit::Link::Tcp, .memory = tracekit::Memory::Small};
    }
    void commands(tracekit::Commands& c) override {
        c.module("relay", "Relé da bomba")
         .add("set", [this](uint8_t on) { return set(on); }, "relay -set <0|1>");
    }
    void topics(tracekit::Topics& t) override {
        relay_ = t.topic("relay", tracekit::Hz(1)).boolean("on", &on_);
    }

private:
    uint8_t set(uint8_t on) {
        on_ = on != 0U;
        gpio_set_level(RELE, on_);
        device().publish(relay_);   // publica imediatamente
        return RESULT_OK;
    }
    bool on_ = false;
    tracekit::Topic relay_;
};

BombaJardim app;
TraceKit device(app);
```

A classe é lida apenas dentro de `begin()`, portanto a ordem de construção dos objetos
globais não importa. Dentro dela, `device()` dá acesso à instância de `TraceKit`.

Também é possível usar estruturas de configuração e funções livres, como nos exemplos
01, 02, 04 e 05:

```cpp
#include <TraceKit.h>
#include "btp_device.h"     // const tracekit::DeviceConfig device_config = {...};
#include "btp_storage.h"    // const tracekit::StorageConfig storage_config = {...};

void app_commands(tracekit::Commands& c);
void app_topics(tracekit::Topics& t);

TraceKit device(device_config, storage_config);

extern "C" void app_main() {
    device.begin(app_commands, app_topics);   // opcionalmente, uma função de settings
}
```

## Configuração

A configuração é feita por duas estruturas com valores padrão; basta informar os campos
que diferem do padrão, na ordem em que são declarados (exigência dos inicializadores
designados do C++20).

### `DeviceConfig`

| Campo | Padrão | Descrição |
|---|---|---|
| `name` | — | Nome do dispositivo (até 29 caracteres). Usado no mDNS, no anúncio BLE e no hostname. |
| `link` | `Serial` | `Serial`, `Tcp`, `Ble`, `EspNow` ou `FromSettings` (valor salvo por `btp -link_set`). |
| `fallback` | `Serial` | Link usado por `FromSettings` quando nenhum valor foi salvo. |
| `memory` | `Medium` | Preset de buffers: `Small`, `Medium` ou `Large` ([Memória](#memória-e-psram)). |
| `threading` | `Auto` | Com `Manual`, a aplicação chama `device.routine()` no próprio laço. |
| `comms` | núcleo 0, prioridade 10, 5 KB | Task de comunicação: `core`, `priority`, `stack_bytes`, `period_ms`, `timer`. |
| `shell` | sem afinidade, 5 KB | Task de execução de comandos. |
| `wifi` | `wifi.txt` | Credenciais fixas, arquivo de redes e política de buffers (`Lean` ou `Sdkconfig`). |
| `tcp` | porta 44300 | Porta do servidor TCP (padrão do TraceView). |
| `espnow` | — | MAC do dongle e canal. |
| `serial_port` | `Auto` | `Usb` (TinyUSB), `UsbJtag`, `Uart` ou `Auto` (seleção pelo chip e pela conexão). |
| `usb` / `uart` | — | Parâmetros do TinyUSB e da UART: porta, baud rate (921600), pinos, silenciamento do console. |
| `serial_cleartext` | `false` | Mantém o link Serial sem criptografia mesmo com chave provisionada. |
| `source_info` | — | Até 8 pares chave/valor adicionais no manifesto (ex.: revisão de hardware). |
| `peer` | — | [Comunicação entre dispositivos](#comunicação-entre-dispositivos). |
| `debug_log` | `false` | Relatório de inicialização no console serial ([Diagnóstico](#diagnóstico)). |
| `time` | Brasília, SNTP ativo | Fuso POSIX, servidor e intervalo do SNTP ([Data e hora](#data-e-hora)). |
| `power` | `Performance` | `Saving` ativa o modo de economia ([Energia](#energia-e-sono-profundo)). |
| `sleep` | — | GPIOs de despertar e de permanência acordado ([Sono profundo](#sono-profundo)). |
| `recovery` | ativo, 3 falhas | Detecção de crash loop: `enabled`, `crash_loop`, `stable_ms`. |
| `psram` | `Buffers` | `Off`, `Buffers` ou `Stacks` ([PSRAM](#memória-e-psram)). |

### `StorageConfig`

| Campo | Padrão | Descrição |
|---|---|---|
| `fs` | partição `storage` em `/fs` | LittleFS. `partition = ""` desativa. |
| `key` | `Nvs` | `Nvs`: importa o arquivo de chave uma única vez para o NVS. `None`: sem chave. |
| `key_file` | `bally.key` | Nome do arquivo de chave procurado na inicialização. |
| `sd` | desativado | Cartão SD: `.driver = tracekit::sd_card`, modo (`Spi`/`Sdmmc`), pinos, ponto de montagem `/sd`. |
| `app_volume` | — | Volume montado pela própria aplicação (ex.: `/sdcard` compartilhado com USB MSC). |

### Opções do menuconfig

| Opção | Descrição |
|---|---|
| `CONFIG_TRACEKIT_LOG_LEVEL` | Nível de log da biblioteca. |
| `CONFIG_TRACEKIT_LINK_SERIAL` / `_TCP` / `_BLE` / `_ESPNOW` | Links incluídos no binário. Links desativados não ocupam flash. |
| `CONFIG_TRACEKIT_INSECURE_CLEARTEXT_RADIO` | **Apenas para desenvolvimento:** permite links por rádio sem chave. |

## Links de comunicação

O dispositivo usa um link por inicialização. Se o link solicitado não puder ser iniciado
(chave ausente, suporte desativado no build ou indisponível no chip), a biblioteca usa
o Serial e registra o motivo no log.

| Link | Implementação | Chave | Observações |
|---|---|---|---|
| **Serial** | USB nativa (TinyUSB CDC), USB-Serial-JTAG ou UART | opcional | `Auto` prioriza a USB nativa, depois a USB-Serial-JTAG (se houver host conectado) e, por fim, a UART. Quando compartilha a porta do console, o `ESP_LOG` é silenciado. |
| **TCP** | Wi-Fi station e servidor TCP na porta 44300 | obrigatória | Acessível por `<nome>.local`. Reconexão automática, alternando entre as redes conhecidas. |
| **BLE** | Periférico NimBLE com o serviço GATT do BTP | obrigatória | Requer NimBLE habilitado no menuconfig. |
| **ESP-NOW** | Datagramas via `bally_dongle` conectado ao PC | obrigatória | Usa a chave L. |

**Redes Wi-Fi.** A lista de redes combina `wifi.ssid`/`wifi.password` (se definidos) com
o arquivo `wifi.txt`, uma rede por linha, até 8 no total:

```text
Casa,minhasenha
Laboratorio,outrasenha
```

As redes podem ser gerenciadas pelo terminal (`btp -wifi`, `btp -wifi_add`,
`btp -wifi_remove`) ou pelo TraceView. Senhas nunca são exibidas.

**Troca de link em campo.** Com `.link = tracekit::Link::FromSettings`, o link é lido do
NVS: `btp -link_set ble` seguido de `btp -reboot`. Pelo código, `device.set_link()`.

## Telemetria

Um tópico é um conjunto de campos publicados juntos. Ele só é transmitido enquanto houver
um assinante, na taxa solicitada (limitada à taxa declarada).

```cpp
void topics(tracekit::Topics& t) override {
    // campos vinculados a variáveis
    t.topic("clima", tracekit::Hz(1))
     .f32("temperatura", &temperatura, "°C", -40.f, 85.f)
     .f32("umidade", &umidade, "%", 0.f, 100.f);

    // ou uma função que preenche uma amostra consistente
    t.topic("energia", tracekit::Hz(0.2))
     .f32("bateria_v", "V")
     .f32("solar_ma", "mA")
     .fill([](tracekit::Sample& s) {
         s.put("bateria_v", le_bateria());
         s.put("solar_ma", le_solar());
     });

    // texto livre (Text Board do TraceView)
    t.text("status", tracekit::Hz(0.5), [](tracekit::Text& out) {
        out.printf("uptime %lus", uptime_s());
    });
}
```

- **Tipos de campo:** `boolean`, `u8`, `i8`, `u16`, `i16`, `u32`, `i32`, `f32`, `f64`
  (apenas com `.fill()`) e `text`, com unidade e faixa opcionais.
- **Taxas:** `tracekit::Hz(10)`, `tracekit::Hz(0.2)` ou `tracekit::OnEvent` (publicação
  apenas via `device.publish()`).
- **Validação:** erros de declaração (nome duplicado, faixa inválida, capacidade esgotada)
  fazem `begin()` retornar `TRACEKIT_ERR_BAD_TOPIC`, identificando o tópico.
- **Handles:** cada declaração retorna um `tracekit::Topic`, usado em `publish()`,
  `subscribed()` e `write_stream()` sem busca por nome.
- **Concorrência:** variáveis vinculadas por ponteiro são lidas pela task de comunicação,
  possivelmente em outro núcleo. `float` e inteiros alinhados de até 4 bytes são lidos
  atomicamente; campos que precisam ser coerentes entre si devem usar `.fill()`.

**Streams.** Sinais contínuos (áudio, vibração) são enviados em blocos de amostras, com
taxa de amostragem e contador de sequência. No TraceView, o widget Audio Spectrum
reproduz o sinal e exibe a FFT.

```cpp
t.stream("audio.pcm", 1024, "counts").range(-2048, 2047);                    // declaração
device.write_stream("audio.pcm", bloco, 1024, 83333, esp_timer_get_time());  // envio
```

**Payloads binários.** Conteúdo com formato próprio (imagens, matrizes compactadas). A
função escreve o payload e retorna o tamanho, ou 0 quando não há dados novos:

```cpp
t.bytes("camera.matrix", tracekit::Hz(30), 1900, copia_ultimo_quadro).version(2);
```

## Comandos e terminal

Comandos são agrupados em módulos e invocados como `modulo -comando args`. Os
argumentos são convertidos de acordo com a assinatura da função:

```cpp
void commands(tracekit::Commands& c) override {
    c.module("relay", "Relé da bomba")
     .add("set", [this](uint8_t on) { return set(on); }, "relay -set <0|1>")
     .add("pulse", [this](uint32_t ms) { return pulse(ms); }, "relay -pulse <ms>");
}
```

Cada comando retorna `RESULT_OK` ou `RESULT_ERROR`. Os comandos são executados em série
na task `shell`, sem bloquear a comunicação.

Funções disponíveis em qualquer ponto do firmware:

```cpp
tracekit::printf("relé ligado\n");                                // terminal do TraceView
tracekit::log(tracekit::LogLevel::Warn, "bateria em %u%%", pct);  // painel de logs
device.publish("relay");                                          // publica o tópico agora
device.run("relay -set 0");                                       // executa um comando
```

**Barra de progresso.** Operações longas podem exibir uma barra atualizada na mesma linha:

```text
dev> sensor -calibrate 200
! calibrate  47% [=============>                ]
```

`tracekit::progress` segue a assinatura de callback C `void (*)(void* ctx, uint8_t pct)`,
de modo que a rotina não depende da TraceKit:

```cpp
c.module("sensor", "Sensor").add("calibrate", [](int amostras) {
    return calibrate(amostras, tracekit::progress, (void*)"calibrate");
});
```

As atualizações são limitadas a cerca de 10 por segundo; o valor de 100% é sempre enviado.

## Configurações persistentes

Parâmetros ajustáveis em campo (setpoints, períodos, pinos) são declarados como
configurações. Os valores ficam no NVS, sobrevivem a reinicializações e ao sono profundo,
e podem ser alterados sem recompilar.

```cpp
void settings(tracekit::Settings& s) override {
    s.f32("setpoint", &setpoint_, 25.0f).range(0, 100).unit("°C").help("temperatura alvo");
    s.u32("period_ms", &period_ms_, 500).range(100, 60000);
    s.u8("led_pin", &pin_, 48).reboot();                 // aplicado apenas na inicialização
    s.text("local", local_, sizeof(local_), "bancada");
    s.boolean("alarme", &alarme_, true).on_change([this] { atualiza_alarme(); });
}
```

Os valores salvos são carregados **antes** de `commands()` e `topics()`. Pelo terminal:

```text
cfg -list                  valor, tipo, padrão, faixa e descrição de cada configuração
cfg -get setpoint
cfg -set setpoint, 30      valida tipo e faixa, salva e chama on_change
cfg -reset setpoint        restaura o padrão (cfg -reset all para todas)
```

- **Tipos:** `boolean`, `u8`, `u16`, `u32`, `i32`, `f32` e `text` (até 64 caracteres).
- **Limites:** 32 configurações; nomes de até 15 caracteres (usados como chave no NVS).
- **API:** `device.set_setting("setpoint", "30")` equivale a `cfg -set`;
  `device.save_setting("setpoint")` persiste um valor alterado diretamente na variável;
  `device.get_setting()` retorna o valor como texto.
- Valores salvos com tipo incompatível ou fora da faixa atual voltam ao padrão, com aviso
  no log.
- `btp -factory_reset yes` preserva as configurações; `btp -factory_reset all` as apaga.

**No TraceView**, o painel de ajustes do dispositivo lista as configurações com o editor
adequado a cada tipo e faixa, além das redes Wi-Fi e das ações de sistema (reinicializar,
recovery, bootloader, reset de fábrica). O painel é gerado a partir da descrição enviada
pelo próprio dispositivo, sem configuração adicional.

## Data e hora

O ESP32 não possui relógio alimentado por bateria: a hora é preservada em
reinicializações e no sono profundo, mas não em caso de falta de energia.

- **Com acesso à internet** (link TCP), a hora é sincronizada por SNTP na inicialização e
  a cada 6 horas.
- **Sem acesso à internet**, a hora é recebida do TraceView ao conectar, ou pode ser
  ajustada pelo terminal ou por um RTC/GPS externo via `device.set_time()`.

```text
btp -time                                 hora local, UTC, fuso e origem
btp -time_set 2026-11-19 14:00            hora local
btp -time_set 2026-11-19T14:00:00-03:00   com fuso explícito (ou segundos desde 1970)
btp -tz <-03>3                            fuso POSIX, salvo no NVS
```

API: `device.time_valid()` indica se a hora é confiável, `device.now()` retorna os segundos
UTC, `device.time_source()` informa a origem (`Sntp`, `Manual`, `Application`), e
`localtime_r()` converte para o fuso configurado.

## Energia e sono profundo

### Modo de economia

```cpp
.power = {.mode = tracekit::PowerMode::Saving},
```

O modo também pode ser alterado em execução com `device.set_power_mode()` ou
`btp -power_set saving`. O Wi-Fi passa a dormir entre os beacons do roteador, a CPU reduz
a frequência e, com as opções abaixo no `sdkconfig.defaults`, o chip entra em light sleep
automaticamente quando ocioso:

```ini
CONFIG_PM_ENABLE=y
CONFIG_FREERTOS_USE_TICKLESS_IDLE=y
CONFIG_PM_PROFILING=y            # opcional: tempo em cada modo, exibido por btp -power
```

A conexão com o TraceView é mantida. Em um ESP32-S3 com link TCP, o chip permaneceu
cerca de 78% do tempo em light sleep, com aumento de aproximadamente 100 ms na latência
das respostas. Com o link Serial, o light sleep não é utilizado para evitar perda de dados.

### Sono profundo

Para dispositivos alimentados por bateria ([exemplo 07](examples/07_sleep_sensor)). Cada
despertar corresponde a uma nova inicialização:

```cpp
extern "C" void app_main() {
    device.begin();
    mede_e_publica();
    device.sleep_for(5 * 60 * 1000);   // 5 minutos; ver também sleep_until(utc)
}
```

- `device.wake_reason()` informa a causa do despertar (`PowerOn`, `Timer`, `Gpio`) e
  `device.wake_count()`, o número de ciclos.
- Hora, configurações e o contador de boots são preservados sem escrita na flash a cada
  ciclo.
- Antes de dormir, a biblioteca aguarda `sleep.flush_ms` para esvaziar as filas e encerra
  o link.
- `sleep.wake_gpio` define um GPIO de despertar; `sleep.awake_gpio`, mantido ativo na
  inicialização, impede o sono.
- `btp -awake <min>` bloqueia o sono pelo período indicado, permitindo uma sessão de
  manutenção ou uma atualização. `btp -awake 0` remove o bloqueio.

## Armazenamento

A biblioteca usa três meios: o **NVS** (chave/valor na flash, tolerante a falhas de
energia), o **LittleFS** (partição `storage`) e, opcionalmente, um **cartão SD**.

| Dado | Local |
|---|---|
| Configurações, fuso horário, link selecionado | NVS |
| Chave do dispositivo (após importação) | NVS |
| Contador de boots | NVS e memória RTC |
| Nome e redes Wi-Fi usados pelo recovery | NVS |
| `wifi.txt` | LittleFS (ou cartão SD, na ausência de LittleFS) |
| `bally.key` antes da importação | Cartão SD, volume da aplicação ou LittleFS |
| Arquivos da aplicação | Volume retornado por `device.fs()` |

O NVS é acessado apenas pelos comandos (`cfg`, `btp -tz`, `btp -link_set`). Arquivos são
enviados ao LittleFS com `pio run -t uploadfs` (pasta `data/` do projeto); `btp -fs`
exibe o espaço ocupado e a lista de arquivos.

**Cartão SD.** O driver FAT (SPI ou SDMMC) é incluído no binário apenas quando
configurado (cerca de 70 KB):

```cpp
tracekit::StorageConfig storage() const override {
    return {.sd = {.driver = tracekit::sd_card, .cs = 10, .mosi = 11, .miso = 13, .sck = 12}};
}
```

**API de arquivos.** `device.fs()` retorna um `tracekit::FileSystem*` (ou `nullptr`) com
`read`, `write`, `append`, `exists`, `remove`, `list` e `usage`. Os caminhos são relativos
à raiz do volume.

> **Desgaste da flash:** cada `append` no LittleFS reescreve o último bloco (4 KB). Para
> registros frequentes, acumule os dados em RAM e grave em lotes.

## Segurança

Cada dispositivo possui duas chaves AES-128, derivadas de senhas:

- **E** — comunicação entre TraceView e dispositivo.
- **L** — comunicação entre dongle e dispositivo (ESP-NOW).

Os links por rádio (TCP, BLE, ESP-NOW) exigem chave; sem ela, o dispositivo opera apenas
pelo Serial e registra no log as instruções de provisionamento. Após o handshake, todas
as mensagens são cifradas com AES-128-GCM.

**Provisionamento:**

```text
python tools/provision_key.py --password-e <senha E> --password-l <senha L>
# copie o bally.key gerado para a pasta data/ do projeto
pio run -t uploadfs
```

Na primeira inicialização, a chave é importada para o NVS e o arquivo é renomeado para
`bally.key.imported`. A mesma senha E deve ser configurada no TraceView.

- `btp -key` exibe o estado da chave (apenas as tags públicas de verificação; a chave
  nunca aparece em logs).
- `btp -key_import` importa um novo arquivo, aplicado na próxima inicialização.
- `btp -key_forget yes` remove a chave.
- `serial_cleartext = true` mantém o link Serial sem criptografia.

> Arquivos `*.key` e `wifi.txt` não devem ser versionados; ambos estão no `.gitignore`.

## Recovery e atualização OTA

Com as tabelas de [partitions/](partitions/), a aplicação ocupa a partição `factory` e
um firmware de **recovery** (~0,7 MB) ocupa a `ota_0`. Com o `extra_scripts` mostrado em
[Início rápido](#início-rápido), `pio run -t upload` grava ambos.

| Tabela | Aplicação | Recovery | LittleFS |
|---|---|---|---|
| `tracekit_4mb.csv` | 2,7 MB | 1 MB | 256 KB |
| `tracekit_8mb.csv` | 5 MB | 1 MB | 1,9 MB |
| `tracekit_16mb.csv` | 8 MB | 1 MB | 6,9 MB |

**Atualização.** O comando `btp -recovery` reinicia o dispositivo no recovery, que se
conecta à mesma rede Wi-Fi e aguarda o firmware em `<nome>.local`. O envio pode ser feito:

- **pelo TraceView**, em *File → Upload Firmware*, que executa todo o processo
  automaticamente usando o endereço informado no manifesto (`ota_endpoint`);
- **pela linha de comando**:
  ```text
  python tools/btp_ota.py meu-sensor.local .pio/build/esp32-s3/firmware.bin
  ```
- **pelo navegador**, em `http://<nome>.local/`.

Comportamento do recovery:

- **Sem rede conhecida**, cria um ponto de acesso `btp-<nome>` com portal cativo
  (`http://192.168.4.1/`).
- **Autenticação:** o upload é assinado com a chave E; a senha não trafega pela rede.
- **Timeout:** sem upload em 10 minutos, retorna à aplicação.
- **Crash loop:** três falhas consecutivas antes de 30 s de operação estável ativam o
  recovery automaticamente (`DeviceConfig::recovery`).
- **Reset de fábrica:** `btp -factory_reset yes` apaga a aplicação e o LittleFS,
  preservando chave, redes Wi-Fi e configurações; `all` apaga também o NVS.
- **Gravação por USB:** `btp -bootloader` reinicia no modo download da ROM, dispensando
  o botão BOOT. Sem gravação em 30 s, a placa retorna à aplicação.

Tempo de espera, ponto de acesso e senha são configuráveis no menuconfig (menu
*TraceKit recovery*). Ver também o [exemplo 06](examples/06_recovery_s3).

## Comunicação entre dispositivos

Um segundo link BTP, independente do link com o TraceView, para troca de mensagens entre
dispositivos sobre um transporte fornecido pela aplicação (rádio, LoRa, cabo).

- **Mensagens por canal:** `device.peer_send(canal, dados, tamanho)` e o callback
  `PeerConfig::on_message`, sem sessão nem assinaturas.
- **Chave de grupo:** derivada de uma senha compartilhada (`btp -peer_password <texto>`).
  Sem senha, nenhuma mensagem é enviada e as recebidas são descartadas.
- **Transporte:** implemente `tracekit::PeerTransport::send()` e entregue os dados
  recebidos com `device.peer_receive()`. `device.max_peer_message()` informa o tamanho
  máximo por datagrama.
- `btp -peer` exibe o estado e os contadores.

## Tasks e núcleos

| Task | Padrão | Função |
|---|---|---|
| `tracekit_comms` | núcleo 0, prioridade 10, 5 KB | Proprietária do nó BTP: recepção, publicação e envio. Executa no núcleo dos rádios. |
| `tracekit_shell` | sem afinidade, prioridade 9, 5 KB | Execução dos comandos, em série. |
| `tracekit_tcp` / `_uart` / `_usj` | núcleo 0 | Leitura do link ativo. |

O núcleo 1 permanece livre para a aplicação; um laço de controle fixado nele não é
interrompido pela biblioteca:

```cpp
.comms = {.core = 0, .period_ms = 2, .timer = true},  // ciclo exato de 2 ms
.shell = {.core = 0},                                 // comandos fora do núcleo 1

xTaskCreatePinnedToCore(control_loop, "control", 4096, nullptr, 20, nullptr, 1);
```

- Sem `.timer = true`, `period_ms` é arredondado para ticks do FreeRTOS (com
  `CONFIG_FREERTOS_HZ=100`, 1 ms equivale a 10 ms).
- Nenhuma função da biblioteca é segura para uso em interrupção. Em callbacks de DMA
  (ADC/I2S), notifique uma task e chame `write_stream()` a partir dela.
- Com `Threading::Manual`, a aplicação chama `device.routine()`; a primeira task a
  chamá-la torna-se a proprietária, e chamadas de outras tasks são recusadas.
- `begin()` executa em uma task temporária de 8 KB, sem exigir aumento da stack do
  `app_main`.

## Memória e PSRAM

**Presets** (`DeviceConfig::memory`) dimensionam o nó BTP e as filas:

| Preset | Tópicos | Campos | Assinaturas | Uso típico |
|---|---|---|---|---|
| `Small` | 4 | 16 | 4 | Sensor simples |
| `Medium` | 8 | 48 | 8 | Padrão |
| `Large` | 16 | 128 | 16 | Muitos tópicos ou manifesto extenso |

**PSRAM.** Com a PSRAM habilitada no `sdkconfig.defaults`, a biblioteca aloca nela os
próprios buffers (`Psram::Buffers`, padrão). Para mover também os buffers do Wi-Fi e do
lwIP — o maior consumo de RAM interna em dispositivos TCP:

```ini
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y                 # módulos N8R8/N16R8; QUAD para PSRAM quad
CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y
```

- `WifiBuffers::Lean` (padrão) reserva 6 buffers de recepção e usa buffers dinâmicos na
  transmissão, evitando os ~51 KB de buffers estáticos que o IDF reservaria com PSRAM.
  `WifiBuffers::Sdkconfig` usa os valores do sdkconfig.
- `Psram::Stacks` move também as stacks das tasks da biblioteca. Tasks com stack na
  PSRAM não podem escrever na flash; como os `.fill()` executam na task de comunicação,
  eles não podem gravar arquivos nem NVS nesse modo.
- `Psram::Off` mantém tudo na RAM interna.

**Consumo de referência** (exemplo 06, ESP32-S3, TCP cifrado): ~1,08 MB de flash para a
aplicação completa (Wi-Fi, lwIP, mbedTLS e mDNS incluídos); ~53 KB de RAM interna
usados pela biblioteca (~46 KB com `Psram::Stacks`), restando ~250 KB livres.

## Diagnóstico

Com `debug_log = true`, `begin()` registra no console serial cada etapa da inicialização,
com o tempo decorrido e o heap consumido, seguidos de um resumo de memória e stack por
task. Essa saída não é enviada ao TraceView.

```text
I (312) tracekit: [  0.012] boot      TraceKit 0.1.0, esp32s3, IDF v6.0.1, fw 1.2.0
I (313) tracekit: [  0.013] identity  source 3a1f22c0, boot #42, MAC 24:6f:28:aa:bb:cc
I (331) tracekit: [  0.031] storage   LittleFS /fs, SD -, app -                  heap   -4.1 KB
I (344) tracekit: [  0.044] buffers   preset Medium                              heap  -61.2 KB
I (420) tracekit: [  0.120] link      TCP started, sealed                        heap  -38.7 KB
I (451) tracekit: [  0.151] ready     in 151 ms
I (451) tracekit: heap      182.4 KB free of 320.0 KB, min 180.9 KB, largest block 110.0 KB
I (451) tracekit: task              core prio  stack peak / total min free
I (451) tracekit: tracekit_comms       0   10    3.1 / 5.0 KB       1.9 KB
I (451) tracekit: tracekit_shell       -    9    1.4 / 5.0 KB       3.6 KB
```

- **`peak`** é o uso máximo de stack observado; se `min free` se aproximar de zero,
  aumente a stack da task.
- As tasks do sistema (wifi, tiT, ipc) aparecem com `CONFIG_FREERTOS_USE_TRACE_FACILITY`.
- O consumo de heap por etapa é aproximado, pois inclui alocações concorrentes de outras
  tasks.

Em execução: `btp -mem` (mesmo relatório), `btp -status` (link, sessão, identidade) e
`btp -errors` (contadores de erro desde a inicialização). Quando o link Serial usa a
porta do console, os logs continuam disponíveis no TraceView via `tracekit::log()`.

## Referência de comandos

Disponíveis em todos os dispositivos, pelo terminal do TraceView ou por
[`btp_term.py`](#ferramentas):

| Comando | Descrição |
|---|---|
| `btp -status` | Link, sessão, identidade e memória |
| `btp -errors` | Contadores de erro desde a inicialização |
| `btp -mem` | Heap e pico de stack por task |
| `btp -topics` | Tópicos declarados |
| `btp -reboot` | Reinicia o dispositivo |
| `btp -link` / `-link_set <serial\|tcp\|ble\|espnow>` | Link atual / link da próxima inicialização |
| `btp -wifi` / `-wifi_add <ssid>, <senha>` / `-wifi_remove <ssid>` | Lista, adiciona ou remove redes |
| `btp -wifi_json` | Redes conhecidas em JSON (apenas nomes) |
| `btp -fs` | Arquivos e espaço em disco |
| `btp -key` / `-key_import` / `-key_forget yes` | Estado, importação e remoção da chave |
| `btp -time` / `-time_set <data>` / `-tz <POSIX>` | Consulta e ajuste de hora e fuso |
| `btp -time_sync <epoch>` | Ajuste de hora pelo TraceView (ignorado se o SNTP estiver sincronizado) |
| `btp -power` / `-power_set <saving\|performance>` | Modo de energia |
| `btp -sleep` / `-awake <min>` | Causa do despertar / bloqueio temporário do sono |
| `btp -recovery` / `-recovery_info` | Reinicia no recovery / partição, versão e falhas |
| `btp -factory_reset <yes\|all>` | Reset de fábrica via recovery |
| `btp -bootloader` | Modo download da ROM por 30 s |
| `btp -peer` / `-peer_password <texto>` | Comunicação entre dispositivos |
| `cfg -list` / `-get` / `-set` / `-reset` | [Configurações persistentes](#configurações-persistentes) |
| `cfg -json <i>` | Configuração de índice `i` em JSON, com o total |

## Referência da API

| Grupo | Métodos de `TraceKit` |
|---|---|
| Ciclo de vida | `begin()`, `state()`, `last_error()`, `routine()`, `tick()` |
| Link | `active_link()`, `connected()`, `set_link()` |
| Telemetria | `publish()`, `subscribed()`, `write_stream()` |
| Terminal | `run()`, `shell()` |
| Configurações | `set_setting()`, `save_setting()`, `get_setting()` |
| Data e hora | `time_valid()`, `now()`, `now_us()`, `time_source()`, `set_time()` |
| Energia e sono | `set_power_mode()`, `power_mode()`, `sleep_for()`, `sleep_until()`, `wake_reason()`, `wake_count()`, `stay_awake()` |
| Arquivos | `fs()` |
| Peer | `peer_send()`, `peer_receive()`, `max_peer_message()`, `peer_stats()`, `set_peer_password()`, `has_peer_key()` |
| Avançado | `node()` (acesso ao `btp::Node` subjacente) |

Funções livres: `tracekit::printf()`, `tracekit::log()` e `tracekit::progress()`.

Os erros de `begin()` são valores `esp_err_t` a partir de `TRACEKIT_ERR_BASE`
(`TRACEKIT_ERR_CONFIG`, `TRACEKIT_ERR_LINK_START`, `TRACEKIT_ERR_NO_KEY`,
`TRACEKIT_ERR_BAD_TOPIC`, entre outros), sempre acompanhados de uma mensagem em
`last_error()`.

## Ferramentas

Scripts Python 3 em [tools/](tools/) (requerem `pip install pyserial cryptography`):

| Ferramenta | Descrição |
|---|---|
| `provision_key.py` | Gera o arquivo `bally.key` a partir das senhas E e L |
| `btp_term.py` | Terminal por linha de comando via serial (`COM8`) ou TCP (`nome.local`), com sessão cifrada (`--password-e` ou `--key-file`) |
| `btp_ota.py` | Envio de firmware ao recovery (`--status`, `--reboot`) |
| `pio_recovery.py` | Script do PlatformIO que compila e grava o recovery na partição `ota_0` |
| `link_echo_test.py` | Teste de vazão e eco dos links (com `test_apps/link_echo`) |

```text
python tools/btp_term.py meu-sensor.local --password-e <senha E> "btp -status" "cfg -list"
```

## Exemplos

| Exemplo | Descrição |
|---|---|
| [01_minimal](examples/01_minimal) | Dispositivo mínimo: terminal e um tópico via USB |
| [02_weather_station](examples/02_weather_station) | Estação meteorológica simulada via Wi-Fi |
| [03_relay_actuator](examples/03_relay_actuator) | Comandos atuando no hardware, com a classe `App` |
| [04_ble_secure](examples/04_ble_secure) | Link BLE cifrado |
| [05_espnow_dongle](examples/05_espnow_dongle) | Link ESP-NOW via `bally_dongle` |
| [06_recovery_s3](examples/06_recovery_s3) | ESP32-S3 sem periféricos externos: sensor de temperatura interno, LED RGB, configurações, economia de energia, PSRAM e recovery |
| [07_sleep_sensor](examples/07_sleep_sensor) | Sensor a bateria com sono profundo e despertar por botão |

Os exemplos usam a biblioteca a partir deste repositório e baixam as demais dependências
no primeiro build:

```text
git clone https://github.com/AlisonTristao/TraceKit.git
pio run -d TraceKit/examples/01_minimal -t upload
```

## Compatibilidade

| | |
|---|---|
| Framework | ESP-IDF 6.0 ou superior |
| Chips | **ESP32-S3** (referência). ESP32 via UART. ESP32-C3 e C6 com suporte previsto (Serial via USB-Serial-JTAG), ainda não validados. |
| Links | Serial, TCP, BLE e ESP-NOW — um por inicialização, além do link peer opcional |

## Documentação adicional

- [CONTRIBUTING.md](CONTRIBUTING.md) — organização do código, build e testes.
- [CHANGELOG.md](CHANGELOG.md) — histórico de versões.
- [docs/PLANO.md](docs/PLANO.md) — decisões de arquitetura e roadmap.

## Licença

Distribuído sob a licença [MIT](LICENSE). © 2026 Alison de Oliveira Tristão.
