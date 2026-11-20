# TraceKit

Biblioteca ESP-IDF que transforma um ESP32 num dispositivo pronto para o
[TraceView](https://github.com/AlisonTristao/TraceView). Ela cuida da telemetria, do
terminal, dos comandos, da criptografia, das configurações persistentes, da hora, do
sono e da atualização pela rede. Você declara o que o dispositivo mede e o que ele sabe
fazer, e a TraceKit faz o resto.

Por baixo, a TraceKit fala o protocolo [BTP](https://github.com/AlisonTristao/BTP) e
expõe um shell [TinyShell](https://github.com/AlisonTristao/TinyShell). Antes ela se
chamava **BTPDevice** ([CHANGELOG](CHANGELOG.md)).

> **Estado:** versão 0.x, em desenvolvimento. O que está descrito aqui funciona e foi
> testado num ESP32-S3; o que ainda falta testar em bancada está em
> [docs/PLANO.md](docs/PLANO.md).

## Sumário

- [O que ela faz](#o-que-ela-faz)
- [Começando](#começando)
- [O dispositivo numa classe](#o-dispositivo-numa-classe)
- [Configuração](#configuração)
- [Links](#links)
- [Telemetria](#telemetria)
- [Comandos e terminal](#comandos-e-terminal)
- [Configurações persistentes](#configurações-persistentes)
- [Hora](#hora)
- [Energia e sono](#energia-e-sono)
- [Onde cada coisa fica guardada](#onde-cada-coisa-fica-guardada)
- [Chave e segurança](#chave-e-segurança)
- [Atualização pela rede (recovery)](#atualização-pela-rede-recovery)
- [Link entre dispositivos (peer)](#link-entre-dispositivos-peer)
- [Tarefas e núcleos](#tarefas-e-núcleos)
- [Memória e PSRAM](#memória-e-psram)
- [Debug e diagnóstico](#debug-e-diagnóstico)
- [Referência: comandos `btp`](#referência-comandos-btp)
- [Referência: API do `TraceKit`](#referência-api-do-tracekit)
- [Ferramentas](#ferramentas)
- [Exemplos](#exemplos)
- [Suporte](#suporte)

## O que ela faz

Todo projeto IoT que fala com o TraceView repete a mesma infraestrutura. A TraceKit é
essa infraestrutura, escrita uma vez:

| Área | O que você ganha |
|---|---|
| **Links** | Serial (USB nativa, USB-Serial-JTAG ou UART), TCP por Wi-Fi, Bluetooth LE e ESP-NOW. Se o link pedido não sobe, ela cai para Serial. |
| **Descoberta** | Um nome só, que aparece no anúncio BLE, no mDNS (`<nome>.local`), no hostname DHCP, na porta USB e no manifesto. O TraceView acha o dispositivo por ele. |
| **Telemetria** | Tópicos declarados uma vez, publicados só enquanto alguém assina e na taxa pedida. Também streams (áudio, vibração) e corpos binários (imagens). |
| **Terminal** | Seus comandos em módulos (`relay -set 1`), `printf` e logs para o TraceView, barra de progresso, e os comandos de sistema `btp -...`. |
| **Segurança** | AES-128-GCM com a chave do dispositivo. Os links por rádio só sobem com chave. |
| **Configurações** | Valores do app (setpoint, período, pino) no NVS, alterados pelo terminal sem recompilar. |
| **Hora** | SNTP quando há internet, ajuste manual, fuso configurável (Brasília por padrão). |
| **Energia** | Economia sem desconectar (modem sleep e light sleep) e sono profundo para sensores a bateria. |
| **Atualização** | Firmware de recovery: OTA autenticado por Wi-Fi, pelo `btp_ota.py` ou por uma página no navegador. Entra sozinho se o app cair em crash loop. |
| **Memória** | Presets de tamanho, buffers e stacks na PSRAM, relatório de heap e stack por task. |
| **Erros** | Cada falha sai no log com a causa e o que fazer; o `begin()` devolve um código e uma mensagem. |

## Começando

### O que precisa

- **ESP-IDF 6.0 ou mais novo**, direto (`idf.py`) ou pelo PlatformIO (`framework = espidf`).
- **C++20**: as configurações usam inicializadores designados.
- Os repositórios **BTP** (v2.51.0+) e **TinyShell** (v2.2.0+), por enquanto como pastas
  irmãs desta:

```text
c:/git/TraceKit     <- este repositório (a pasta TEM de se chamar TraceKit)
c:/git/BTP
c:/git/TinyShell
```

O ESP-IDF dá ao componente o nome da pasta, e os projetos pedem `REQUIRES TraceKit`. Para
usar BTP e TinyShell de outro lugar, passe `-DTRACEKIT_BTP_DIR=...` e
`-DTRACEKIT_TINYSHELL_DIR=...`.

### Um projeto novo

O jeito mais rápido é copiar um dos [exemplos](#exemplos). A estrutura mínima:

```cmake
# CMakeLists.txt do projeto
cmake_minimum_required(VERSION 3.16)
include(${CMAKE_CURRENT_LIST_DIR}/../../test_apps/tracekit_components.cmake)
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
board_build.partitions = ../../partitions/tracekit_16mb.csv   ; 4, 8 ou 16 MB
board_build.filesystem = littlefs
extra_scripts = post:../../tools/pio_recovery.py              ; grava o recovery junto
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
    device.begin();                 // ESP_OK, ou a causa no log e em device.last_error()
    for (;;) {
        temperatura = le_sensor();  // o seu código; a lib publica sozinha
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
```

```text
pio run -t upload        # app + recovery
pio run -t uploadfs      # a pasta data/ vai para o LittleFS (chave, wifi.txt)
```

Ligue a USB da placa no PC e abra a porta no TraceView: o dispositivo aparece como
`meu-sensor`. Sem a chave, só o link Serial funciona; para Wi-Fi ou BLE, veja
[Chave e segurança](#chave-e-segurança).

## O dispositivo numa classe

O dispositivo inteiro cabe numa classe derivada de `tracekit::App`, com o estado como
membro ([exemplo 03](examples/03_relay_actuator)). Só `config()` é obrigatório:

| Método | Para quê |
|---|---|
| `config()` | Nome, link, memória, rede, energia, sono... ([`DeviceConfig`](#deviceconfig)) |
| `storage()` | Sistema de arquivos, cartão SD e chave ([`StorageConfig`](#storageconfig)) |
| `settings(Settings&)` | Valores persistentes ([Configurações persistentes](#configurações-persistentes)) |
| `commands(Commands&)` | O que o terminal pode chamar ([Comandos](#comandos-e-terminal)) |
| `topics(Topics&)` | O que o TraceView pode plotar ([Telemetria](#telemetria)) |

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
        device().publish(relay_);   // amostra agora, pelo handle do tópico
        return RESULT_OK;
    }
    bool on_ = false;
    tracekit::Topic relay_;
};

BombaJardim app;
TraceKit device(app);
```

A classe só é lida dentro do `begin()`, nunca num construtor, então a ordem dos dois
globais não importa. Dentro dela, `device()` dá acesso ao `TraceKit`.

**Estilo em arquivos separados.** Também dá para passar as configurações e funções
soltas, como nos exemplos 01, 02, 04 e 05:

```cpp
#include <TraceKit.h>
#include "btp_device.h"     // const tracekit::DeviceConfig device_config = {...};
#include "btp_storage.h"    // const tracekit::StorageConfig storage_config = {...};

void app_commands(tracekit::Commands& c);   // btp_commands.cpp
void app_topics(tracekit::Topics& t);       // btp_topics.cpp

TraceKit device(device_config, storage_config);

extern "C" void app_main() {
    device.begin(app_commands, app_topics);   // e, opcional, uma função de settings
}
```

## Configuração

São duas estruturas com valores padrão razoáveis: escreva só o que muda. Os campos seguem
a ordem da declaração, uma exigência dos inicializadores designados do C++20.

### `DeviceConfig`

| Campo | Padrão | O que é |
|---|---|---|
| `name` | — | Nome do dispositivo (até 29 caracteres). Vira `<nome>.local`, anúncio BLE, hostname. |
| `link` | `Serial` | `Serial`, `Tcp`, `Ble`, `EspNow` ou `FromSettings` (o salvo por `btp -link_set`). |
| `fallback` | `Serial` | Link usado por `FromSettings` quando nada foi salvo. |
| `memory` | `Medium` | Preset de buffers: `Small`, `Medium`, `Large` ([Memória](#memória-e-psram)). |
| `threading` | `Auto` | `Manual`: você chama `device.routine()` no seu laço. |
| `comms` | núcleo 0, prio 10, 5 KB | Task de comunicação: `core`, `priority`, `stack_bytes`, `period_ms`, `timer`. |
| `shell` | sem afinidade, 5 KB | Task que executa os comandos. |
| `wifi` | `wifi.txt` | `ssid`/`password` fixos, arquivo de redes, `buffers` (`Lean` ou `Sdkconfig`). |
| `tcp` | porta 44300 | Porta do servidor TCP (a padrão do TraceView). |
| `espnow` | — | MAC do dongle e canal. |
| `serial_port` | `Auto` | `Usb` (TinyUSB), `UsbJtag`, `Uart`, ou `Auto` (escolhe pelo chip e pelo cabo). |
| `usb` / `uart` | — | Driver TinyUSB; porta, baud (921600), pinos e se o console fica mudo. |
| `serial_cleartext` | `false` | Serial sem criptografia mesmo com chave (quem tem o cabo já tem a placa). |
| `source_info` | — | Até 8 pares chave/valor extras no manifesto (ex.: versão de hardware). |
| `peer` | — | [Link entre dispositivos](#link-entre-dispositivos-peer). |
| `debug_log` | `false` | Relatório do boot no console serial ([Debug](#debug-e-diagnóstico)). |
| `time` | Brasília, SNTP | Fuso POSIX, SNTP liga/desliga, servidor, intervalo ([Hora](#hora)). |
| `power` | `Performance` | `Saving` liga a economia ([Energia](#energia-e-sono)). |
| `sleep` | — | GPIO de despertar e de manter acordado ([Sono profundo](#sono-profundo)). |
| `recovery` | ligado, 3 crashes | Detector de crash loop: `enabled`, `crash_loop`, `stable_ms`. |
| `psram` | `Buffers` | `Off`, `Buffers` ou `Stacks` ([PSRAM](#memória-e-psram)). |

### `StorageConfig`

| Campo | Padrão | O que é |
|---|---|---|
| `fs` | partição `storage` em `/fs` | LittleFS. `partition = ""` desliga. |
| `key` | `Nvs` | `Nvs`: importa o arquivo da chave uma vez e guarda no NVS. `None`: sem chave. |
| `key_file` | `bally.key` | Nome do arquivo procurado no boot. |
| `sd` | desligado | Cartão SD: `.driver = tracekit::sd_card`, modo (`Spi`/`Sdmmc`), pinos, `/sd`. |
| `app_volume` | — | Um volume que o próprio app monta (ex.: `/sdcard` compartilhado com USB MSC). |

### Opções do menuconfig

| Opção | O que faz |
|---|---|
| `CONFIG_TRACEKIT_LOG_LEVEL` | Nível dos logs da lib. |
| `CONFIG_TRACEKIT_LINK_SERIAL` / `_TCP` / `_BLE` / `_ESPNOW` | Quais links entram no binário (o que fica fora não ocupa flash). |
| `CONFIG_TRACEKIT_INSECURE_CLEARTEXT_RADIO` | **Só desenvolvimento:** links por rádio sem chave. |

## Links

Um link por boot. Se o pedido não sobe (sem chave, sem suporte no build ou no chip), a
lib cai para Serial e diz o motivo no log: sem Serial não haveria como corrigir.

| Link | Como | Chave | Observações |
|---|---|---|---|
| **Serial** | USB nativa (TinyUSB CDC), USB-Serial-JTAG ou UART | opcional | `Auto` usa a USB nativa se o build tem TinyUSB; senão a USB-Serial-JTAG, se há um PC nela; senão a UART. Na porta do console, o `ESP_LOG` fica mudo enquanto o link roda. |
| **TCP** | Wi-Fi station + servidor TCP na porta 44300 | obrigatória | Encontrado como `<nome>.local`. Reconecta sozinho e percorre as redes conhecidas. |
| **BLE** | Periférico NimBLE com o serviço GATT do BTP | obrigatória | Precisa do NimBLE ligado no menuconfig. |
| **ESP-NOW** | Datagramas pelo `bally_dongle` ligado no PC | obrigatória | Usa a chave L. |

**Redes Wi-Fi.** A lib junta a rede de `wifi.ssid`/`wifi.password` (se houver) com as do
arquivo `wifi.txt`, uma por linha, até 8 no total:

```text
Casa,minhasenha
Laboratorio,outrasenha
```

O `btp -wifi_add <ssid>, <senha>` acrescenta uma rede ao arquivo, e o `btp -wifi` lista
os nomes (nunca as senhas).

**Trocar o link sem regravar.** Com `.link = tracekit::Link::FromSettings`, o link vem do
NVS: `btp -link_set ble` e depois `btp -reboot`. No código, `device.set_link()`.

## Telemetria

Um tópico é um grupo de campos publicado junto. Ele só é enviado enquanto o TraceView (ou
outro cliente) está assinando, na taxa pedida, até a declarada.

```cpp
void topics(tracekit::Topics& t) override {
    // campos ligados a variáveis: nada mais a escrever
    t.topic("clima", tracekit::Hz(1))
     .f32("temperatura", &temperatura, "°C", -40.f, 85.f)
     .f32("umidade", &umidade, "%", 0.f, 100.f);

    // ou uma função que preenche um retrato consistente
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
  (só com `.fill()`) e `text`, cada um com unidade e faixa opcionais.
- **Taxas:** `tracekit::Hz(10)`, `Hz(0.2)`, ou `tracekit::OnEvent` (só quando você chama
  `device.publish()`).
- **Erros de declaração** (nome repetido, faixa invertida, pool cheio) fazem o `begin()`
  devolver `TRACEKIT_ERR_BAD_TOPIC`, com o nome do tópico.
- **Handle:** toda declaração vira um `tracekit::Topic`. Com ele, `device.publish(topic)`,
  `subscribed(topic)` e `write_stream(topic, ...)` não procuram pelo nome.
- **Variáveis ligadas por ponteiro** são lidas pela task de comunicação, possivelmente no
  outro núcleo. `float` e inteiros de até 4 bytes alinhados são lidos de forma atômica;
  campos que precisam ser coerentes entre si vão num `.fill()`.

**Streams.** Sinais contínuos (áudio, vibração) vão em blocos de amostras. Cada bloco chega
ao TraceView uma vez, em ordem, com a taxa e um contador, e o widget Audio Spectrum toca e
mostra a FFT:

```cpp
t.stream("audio.pcm", 1024, "counts").range(-2048, 2047);                    // declaração
device.write_stream("audio.pcm", bloco, 1024, 83333, esp_timer_get_time());  // de uma task
```

**Bytes.** Um corpo que só o receptor sabe ler (uma imagem, uma grade compactada). A
função escreve o corpo e devolve o tamanho, ou 0 quando não há nada novo:

```cpp
t.bytes("camera.matrix", tracekit::Hz(30), 1900, copia_ultimo_quadro).version(2);
```

## Comandos e terminal

Os comandos ficam em módulos e são chamados no terminal do TraceView como
`modulo -comando args`. Os argumentos são convertidos pelos tipos da função:

```cpp
void commands(tracekit::Commands& c) override {
    c.module("relay", "Relé da bomba")
     .add("set", [this](uint8_t on) { return set(on); }, "relay -set <0|1>")
     .add("pulse", [this](uint32_t ms) { return pulse(ms); }, "relay -pulse <ms>");
}
```

A função devolve `RESULT_OK` ou `RESULT_ERROR`. Os comandos rodam um de cada vez, na task
`shell`, sem travar a comunicação.

De qualquer ponto do firmware:

```cpp
tracekit::printf("relé ligado\n");                                // terminal do TraceView
tracekit::log(tracekit::LogLevel::Warn, "bateria em %u%%", pct);  // painel de logs
device.publish("relay");                                          // amostra agora
device.run("relay -set 0");                                       // um comando, como se digitado
```

**Barra de progresso.** Um trabalho longo mostra uma barra redesenhada na mesma linha:

```text
dev> sensor -calibrate 200
! calibrate  47% [=============>                ]
```

`tracekit::progress` tem o formato de um callback C (`void (*)(void* ctx, uint8_t pct)`),
então a função do trabalho não precisa conhecer a TraceKit:

```cpp
c.module("sensor", "Sensor").add("calibrate", [](int amostras) {
    return calibrate(amostras, tracekit::progress, (void*)"calibrate");
});
```

Pode chamar com a frequência que quiser: o link recebe no máximo ~10 redesenhos por
segundo, e o 100% sempre sai.

## Configurações persistentes

O que precisa ser ajustado em campo (um setpoint, um período, um pino) vira uma
configuração. Ela fica no NVS, sobrevive a reboot e a sono profundo, e muda pelo terminal
sem recompilar.

```cpp
void settings(tracekit::Settings& s) override {
    s.f32("setpoint", &setpoint_, 25.0f).range(0, 100).unit("°C").help("temperatura alvo");
    s.u32("period_ms", &period_ms_, 500).range(100, 60000);
    s.u8("led_pin", &pin_, 48).reboot();                 // lido só no boot
    s.text("local", local_, sizeof(local_), "bancada");
    s.boolean("alarme", &alarme_, true).on_change([this] { atualiza_alarme(); });
}
```

A lib carrega os valores salvos **antes** de `commands()` e `topics()`, então as variáveis
já estão certas quando o seu código roda. No terminal:

```text
cfg -list                  valor, tipo, padrão, faixa e ajuda de cada uma
cfg -get setpoint
cfg -set setpoint, 30      confere tipo e faixa, salva e chama o on_change
cfg -reset setpoint        volta ao padrão (cfg -reset all)
```

- **Tipos:** `boolean`, `u8`, `u16`, `u32`, `i32`, `f32`, `text` (até 64 caracteres).
- **Limites:** 32 configurações, nomes de até 15 caracteres (o nome é a chave do NVS).
- **No código:** `device.set_setting("setpoint", "30")` faz o mesmo que o `cfg -set`;
  `device.save_setting("setpoint")` salva um valor que o app mudou direto na variável;
  `device.get_setting()` lê como texto.
- Um valor salvo com outro tipo, ou fora da faixa nova, volta ao padrão, com aviso.
- `btp -factory_reset yes` mantém as configurações; `all` apaga.

**No TraceView**, o ícone de ajustes no card de um dispositivo TraceKit abre um painel com
estas configurações já com o editor certo para cada tipo e faixa (`cfg -json`), as redes
Wi-Fi (adicionar e tirar) e as ações (`btp -reboot`, `-recovery`, `-bootloader`, `-awake`,
reset de fábrica). Nada a configurar: cada dispositivo descreve as suas.

## Hora

O ESP32 não tem relógio com bateria. A hora sobrevive a reboot e a sono profundo, mas não
a falta de energia.

- **Com internet** (link TCP), a lib acerta sozinha por SNTP, e de novo a cada 6 h.
- **Sem internet**, pelo terminal, ou por um RTC/GPS com `device.set_time()`.

```text
btp -time                                 hora local, UTC, fuso e de onde veio
btp -time_set 2026-11-19 14:00            hora local
btp -time_set 2026-11-19T14:00:00-03:00   com fuso; ou segundos desde 1970
btp -tz <-03>3                            fuso POSIX, salvo no NVS (Brasília é o padrão)
```

No código: `device.time_valid()` diz se a hora é confiável (confira antes de gravar algo
com data), `device.now()` dá os segundos UTC, `device.time_source()` diz de onde veio
(`Sntp`, `Manual`, `Application`) e `localtime_r()` converte para o fuso.

## Energia e sono

### Economia sem desconectar

```cpp
.power = {.mode = tracekit::PowerMode::Saving},
```

Ou `device.set_power_mode()` / `btp -power_set saving`, a qualquer hora. O Wi-Fi dorme
entre os beacons do roteador, a CPU desce para a frequência do cristal e, com estas linhas
no `sdkconfig.defaults`, o chip entra em light sleep sozinho quando nada roda:

```ini
CONFIG_PM_ENABLE=y
CONFIG_FREERTOS_USE_TICKLESS_IDLE=y
CONFIG_PM_PROFILING=y            # opcional: tempo em cada modo no btp -power
```

O TraceView continua conectado. Num S3 com TCP, o chip passou ~78% do tempo em light
sleep, e as respostas ficaram ~100 ms mais lentas. No link Serial o chip não entra em
light sleep (perderia bytes). O `btp -power` mostra o estado.

### Sono profundo

Para sensores a bateria ([exemplo 07](examples/07_sleep_sensor)). Cada despertar é um boot
novo:

```cpp
extern "C" void app_main() {
    device.begin();
    mede_e_publica();
    device.sleep_for(5 * 60 * 1000);   // dorme 5 min; também há sleep_until(utc)
}
```

- `device.wake_reason()` diz por que acordou (`PowerOn`, `Timer`, `Gpio`), e
  `device.wake_count()`, quantas vezes.
- A hora e as configurações sobrevivem ao sono. O `boot_id` segue em sequência sem gravar
  na flash a cada despertar.
- Antes de dormir, a lib espera `sleep.flush_ms` para as filas saírem e fecha o link.
- **Botões:** `sleep.wake_gpio` acorda o dispositivo; `sleep.awake_gpio`, segurado no
  boot, o mantém acordado.
- **Alcançar um dispositivo que dorme:** mande `btp -awake 10` numa janela acordada, e o
  `sleep_for()` recusa por 10 minutos (tempo para uma sessão ou um `btp -recovery`).
  `btp -awake 0` libera.

## Onde cada coisa fica guardada

Há três lugares: o **NVS** (pares chave/valor na flash, resistente a queda de energia), o
**LittleFS** (sistema de arquivos na partição `storage`) e o **cartão SD** (opcional).

| O quê | Onde |
|---|---|
| Configurações do app (`cfg`), fuso, link escolhido | NVS |
| Chave do dispositivo, depois de importada | NVS (nunca num arquivo legível) |
| Contador de boots | NVS + memória do RTC |
| Nome e redes Wi-Fi para o recovery | NVS (o recovery não monta sistema de arquivos) |
| `wifi.txt` | Arquivo: LittleFS; o cartão só quando não há LittleFS |
| `bally.key` antes de importar | Arquivo: procurado no cartão, no volume do app e no LittleFS |
| Arquivos do app | Onde `device.fs()` apontar: LittleFS, senão o cartão, senão o volume do app |

Você não mexe no NVS diretamente: os comandos (`cfg`, `btp -tz`, `btp -link_set`) são a
interface. Os arquivos vão para o LittleFS com `pio run -t uploadfs` (a pasta `data/` do
projeto), e o `btp -fs` mostra o espaço e a lista.

**Cartão SD.** O driver (FAT, por SPI ou SDMMC) só entra no binário quando é usado, e
custa ~70 KB:

```cpp
tracekit::StorageConfig storage() const override {
    return {.sd = {.driver = tracekit::sd_card, .cs = 10, .mosi = 11, .miso = 13, .sck = 12}};
}
```

**API de arquivos.** `device.fs()` devolve um `tracekit::FileSystem*` (ou `nullptr`) com
`read`, `write`, `append`, `exists`, `remove`, `list` e `usage`. Os caminhos são relativos
à raiz do volume.

> **Cuidado com logs no LittleFS:** acrescentar linha a linha copia o último bloco (4 KB)
> a cada linha e desgasta a flash. Junte as linhas na RAM e grave em pedaços. Um log em
> arquivo pronto está nos planos ([PLANO](docs/PLANO.md#depois-da-v1)).

## Chave e segurança

Cada dispositivo tem duas chaves AES-128, derivadas de duas senhas:

- **E**: TraceView ↔ dispositivo.
- **L**: dongle ↔ dispositivo (ESP-NOW).

Os links por rádio (TCP, BLE, ESP-NOW) só sobem com a chave. Sem ela, o dispositivo cai
para Serial e diz no log como provisionar. Toda mensagem depois do HELLO vai selada com
AES-128-GCM.

```text
python tools/provision_key.py --password-e <senha E> --password-l <senha L>
# copie o bally.key gerado para a pasta data/ do projeto e:
pio run -t uploadfs
```

No primeiro boot a chave vai para o NVS, e o arquivo vira `bally.key.imported`. A mesma
senha E é configurada no TraceView para o dispositivo.

- `btp -key` mostra a situação, só com as tags públicas de verificação. A chave nunca
  aparece em log, nem em parte.
- `btp -key_import` importa um arquivo novo sem reiniciar (vale no próximo boot).
- `btp -key_forget yes` apaga a chave.
- `serial_cleartext = true` deixa o Serial em claro mesmo com chave.

> **Nunca faça commit** de `*.key` nem de `wifi.txt`. O `.gitignore` já cobre os dois.

## Atualização pela rede (recovery)

Com uma das tabelas de [partitions/](partitions/), o app fica na partição `factory` e um
firmware pequeno de **recovery** (~0,7 MB) fica na `ota_0`. O `pio run -t upload` grava os
dois quando o `platformio.ini` tem o `extra_scripts` mostrado em
[Um projeto novo](#um-projeto-novo).

| Tabela | App | Recovery | LittleFS |
|---|---|---|---|
| `tracekit_4mb.csv` | 2,7 MB | 1 MB | 256 KB |
| `tracekit_8mb.csv` | 5 MB | 1 MB | 1,9 MB |
| `tracekit_16mb.csv` | 8 MB | 1 MB | 6,9 MB |

**Atualizar.** Mande `btp -recovery` pelo terminal. O dispositivo reinicia no recovery,
entra no mesmo Wi-Fi do app e espera o firmware como `<nome>.local`:

```text
python tools/btp_ota.py meu-sensor.local .pio/build/esp32-s3/firmware.bin
```

Ou pelo navegador: `http://<nome>.local/` abre uma página para escolher o `.bin`.

**Pelo TraceView**, a aba OTA (*File → Upload Firmware*) faz tudo com um clique: com o
dispositivo conectado, ela manda `btp -recovery`, espera o recovery e envia o firmware,
assinado com a mesma senha da conexão. O endereço vem do manifesto (`ota_endpoint`, que a
lib preenche com `<nome>.local` quando há recovery).

- **Sem rede conhecida**, o recovery cria a própria, `btp-<nome>`. Conecte o celular nela
  e a página abre sozinha (ou abra `http://192.168.4.1/`).
- **Autenticação:** o upload é autenticado pela chave E. A senha é digitada no PC ou na
  página e nunca trafega.
- **Sem upload**, o recovery volta ao app em 10 minutos.
- **Crash loop:** um app que trava 3 vezes seguidas antes de ficar estável (30 s) cai
  sozinho no recovery (`DeviceConfig::recovery`).
- **Reset de fábrica:** `btp -factory_reset yes` apaga o app e o LittleFS e mantém chave,
  Wi-Fi e configurações; `all` apaga também o NVS.
- **Gravar por USB sem o botão BOOT:** `btp -bootloader` reinicia no modo download da
  ROM. Sem gravação, a placa volta ao app em 30 s.

O recovery tem opções no menuconfig (menu *TraceKit recovery*): o tempo de espera, a rede
própria e a senha dela. Mais detalhes em
[docs/PLANO.md](docs/PLANO.md#414-recovery-e-ota) e no
[exemplo 06](examples/06_recovery_s3).

## Link entre dispositivos (peer)

É um segundo link BTP, de dispositivo para dispositivo, sobre um transporte que o app
fornece (rádio, LoRa, um cabo). O link do TraceView continua o mesmo.

- **Mensagens por canal:** `device.peer_send(canal, dados, tamanho)` e
  `PeerConfig::on_message`, sem sessão nem assinaturas.
- **Chave do grupo:** derivada de uma senha que todos os dispositivos compartilham
  (`btp -peer_password <texto>`). Sem senha, nada sai e tudo que chega é descartado.
- **Transporte:** implemente `tracekit::PeerTransport::send()` e entregue o que chegar com
  `device.peer_receive()`. Cada mensagem cabe num datagrama; `device.max_peer_message()`
  diz quanto.
- O `btp -peer` mostra a situação e os contadores.

## Tarefas e núcleos

| Task | Padrão | O que faz |
|---|---|---|
| `tracekit_comms` | núcleo 0, prioridade 10, 5 KB | Dona única do nó BTP: recebe, publica, envia. Fica no núcleo dos rádios. |
| `tracekit_shell` | sem afinidade, prioridade 9, 5 KB | Executa os comandos, um de cada vez. |
| `tracekit_tcp` / `_uart` / `_usj` | núcleo 0 | Leitura do link. |

O núcleo 1 fica livre para o seu código. Um laço de controle fixado nele nunca é
interrompido pela lib:

```cpp
.comms = {.core = 0, .period_ms = 2, .timer = true},  // passada exata a cada 2 ms
.shell = {.core = 0},                                 // comandos pesados longe do núcleo 1

xTaskCreatePinnedToCore(control_loop, "control", 4096, nullptr, 20, nullptr, 1);
```

- **`period_ms` sem `timer`** é arredondado para ticks do FreeRTOS: com
  `CONFIG_FREERTOS_HZ=100`, 1 ms vira 10 ms. Com `.timer = true`, o período é exato.
- **Nada da lib roda em interrupção.** Num callback de DMA do ADC/I2S, notifique uma task
  e chame `write_stream()` de lá.
- **Com `Threading::Manual`**, você chama `device.routine()` do seu laço. A primeira task
  que chama vira a dona; chamadas de outra task são recusadas.
- O `begin()` roda numa task temporária de 8 KB, então a stack do `app_main` não precisa
  crescer.

## Memória e PSRAM

**Presets** (`DeviceConfig::memory`) dimensionam o nó BTP e as filas:

| Preset | Tópicos | Campos | Assinaturas | Para |
|---|---|---|---|---|
| `Small` | 4 | 16 | 4 | Um sensor simples |
| `Medium` | 8 | 48 | 8 | O padrão |
| `Large` | 16 | 128 | 16 | Muitos tópicos, manifesto grande |

**PSRAM** (S3 N8R8, N16R8...). Ligue-a no `sdkconfig.defaults`, e a lib põe nela os
buffers dela (`Psram::Buffers`, o padrão). O maior consumidor de RAM interna de um
dispositivo TCP é o Wi-Fi, e só o sdkconfig o manda para a PSRAM:

```ini
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y                 # N8R8/N16R8; QUAD nas de PSRAM quad
CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y   # buffers do Wi-Fi e do lwIP na PSRAM
```

- `WifiBuffers::Lean` (o padrão) reserva só 6 buffers de recepção e transmite com buffers
  dinâmicos. Com PSRAM ligada, o IDF reservaria ~51 KB de buffers estáticos.
  `WifiBuffers::Sdkconfig` volta ao que o sdkconfig diz.
- `Psram::Stacks` manda também as stacks das tasks da lib. Uma task com a stack na PSRAM
  não pode escrever na flash. As da lib nunca escrevem, mas os `.fill()` dos tópicos
  rodam na task de comunicação: com `Stacks`, eles também não podem gravar arquivo nem
  NVS.
- `Psram::Off` deixa tudo na RAM interna.

**Quanto custa** (exemplo 06, ESP32-S3, TCP selado): o app inteiro ocupa ~1,08 MB de
flash, com Wi-Fi, lwIP, mbedTLS e mDNS. A lib usa ~53 KB de RAM interna (~46 KB com
`Psram::Stacks`), e sobram ~250 KB livres.

## Debug e diagnóstico

Com `debug_log = true`, o `begin()` descreve o boot no **console serial** (o `ESP_LOG` de
sempre). Isso nunca vai para o TraceView. Cada etapa mostra o tempo desde o boot e o heap
que consumiu, e no fim sai a memória e a stack de cada task:

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

- **`peak`** é o máximo de stack que a task já usou. Se o `min free` chegar perto de zero,
  aumente a stack dela.
- **As tasks do sistema** (wifi, tiT, ipc...) entram na tabela com
  `CONFIG_FREERTOS_USE_TRACE_FACILITY`.
- **O heap por etapa é uma estimativa:** outra task alocando ao mesmo tempo também entra
  na conta.

A qualquer hora, pelo terminal: `btp -mem` (o mesmo relatório), `btp -status` (link,
sessão, identidade) e `btp -errors` (contadores de erro desde o boot).

Com o link Serial na mesma porta do console, o `ESP_LOG` fica mudo enquanto o link roda.
Os logs continuam chegando ao TraceView por `tracekit::log()`.

## Referência: comandos `btp`

Disponíveis em todo dispositivo, no terminal do TraceView (ou pelo
[`btp_term.py`](#ferramentas)):

| Comando | O que faz |
|---|---|
| `btp -status` | Link, sessão, identidade e memória |
| `btp -errors` | Contadores de erro desde o boot |
| `btp -mem` | Heap e o pico de stack de cada task |
| `btp -topics` | Tópicos declarados |
| `btp -reboot` | Reinicia |
| `btp -link` / `-link_set <serial\|tcp\|ble\|espnow>` | Link em uso / salva o do próximo boot |
| `btp -wifi` / `-wifi_add <ssid>, <senha>` / `-wifi_remove <ssid>` | Redes conhecidas (só nomes) / acrescenta / tira uma |
| `btp -wifi_json` | As redes em JSON, para o TraceView (só nomes) |
| `btp -fs` | Arquivos e espaço |
| `btp -key` / `-key_import` / `-key_forget yes` | Situação da chave / importa o arquivo / apaga |
| `btp -time` / `-time_set <data>` / `-tz <POSIX>` | Hora / acerta / fuso |
| `btp -time_sync <epoch>` | A hora do TraceView, a menos que o SNTP já tenha acertado (o TraceView manda ao conectar) |
| `btp -power` / `-power_set <saving\|performance>` | Modo de energia |
| `btp -sleep` / `-awake <min>` | Por que acordou / impede o sono por N minutos |
| `btp -recovery` / `-recovery_info` | Reinicia no recovery / partição, versão, crashes |
| `btp -factory_reset <yes\|all>` | Apaga o app (e o NVS, com `all`) pelo recovery |
| `btp -bootloader` | Modo download da ROM por 30 s |
| `btp -peer` / `-peer_password <texto>` | Link entre dispositivos |
| `cfg -list` / `-get` / `-set` / `-reset` | [Configurações](#configurações-persistentes) |
| `cfg -json <i>` | A configuração `i` em JSON, com o total, para o TraceView |

## Referência: API do `TraceKit`

| Grupo | Métodos |
|---|---|
| Ciclo de vida | `begin()`, `state()`, `last_error()`, `routine()` e `tick()` (modo `Manual`) |
| Link | `active_link()`, `connected()`, `set_link()` |
| Telemetria | `publish()`, `subscribed()`, `write_stream()` |
| Terminal | `run()`, `shell()` |
| Configurações | `set_setting()`, `save_setting()`, `get_setting()` |
| Hora | `time_valid()`, `now()`, `now_us()`, `time_source()`, `set_time()` |
| Energia e sono | `set_power_mode()`, `power_mode()`, `sleep_for()`, `sleep_until()`, `wake_reason()`, `wake_count()`, `stay_awake()` |
| Arquivos | `fs()` |
| Peer | `peer_send()`, `peer_receive()`, `max_peer_message()`, `peer_stats()`, `set_peer_password()`, `has_peer_key()` |
| Avançado | `node()` (o `btp::Node` por baixo) |

Funções livres: `tracekit::printf()`, `tracekit::log()`, `tracekit::progress()`. Os erros
do `begin()` são códigos `esp_err_t` a partir de `TRACEKIT_ERR_BASE` (`TRACEKIT_ERR_CONFIG`,
`_LINK_START`, `_NO_KEY`, `_BAD_TOPIC`...), sempre com uma mensagem em `last_error()`.

## Ferramentas

Em [tools/](tools/), em Python 3 (`pip install pyserial cryptography`):

| Ferramenta | Para quê |
|---|---|
| `provision_key.py` | Gera o `bally.key` a partir das senhas E e L |
| `btp_term.py` | Terminal pela linha de comando, por serial (`COM8`) ou TCP (`nome.local`), com sessão selada (`--password-e` ou `--key-file`) |
| `btp_ota.py` | Envia um firmware ao recovery (`--status`, `--reboot`) |
| `pio_recovery.py` | Script do PlatformIO que compila o recovery e o grava na `ota_0` |
| `link_echo_test.py` | Teste de vazão e eco dos links (com `test_apps/link_echo`) |

```text
python tools/btp_term.py meu-sensor.local --password-e <senha E> "btp -status" "cfg -list"
```

## Exemplos

| Exemplo | O que mostra |
|---|---|
| [01_minimal](examples/01_minimal) | O menor dispositivo: terminal e um tópico pela USB |
| [02_weather_station](examples/02_weather_station) | Sensores simulados por Wi-Fi |
| [03_relay_actuator](examples/03_relay_actuator) | Comandos que agem no hardware, tudo numa classe `App` |
| [04_ble_secure](examples/04_ble_secure) | BLE com a chave do dispositivo |
| [05_espnow_dongle](examples/05_espnow_dongle) | ESP-NOW pelo `bally_dongle` |
| [06_recovery_s3](examples/06_recovery_s3) | ESP32-S3 sem nada ligado: sensor de temperatura interno, LED RGB, configurações, economia de energia, PSRAM e recovery |
| [07_sleep_sensor](examples/07_sleep_sensor) | Sensor a bateria: acorda, mede, grava, publica e dorme; o botão BOOT acorda |

## Suporte

| | |
|---|---|
| Framework | ESP-IDF 6.0+ (componente) |
| Chips | **ESP32-S3** é o principal e o mais testado. ESP32 clássico pelo Serial na UART. ESP32-C3 e C6 estão previstos no código (Serial por USB-Serial-JTAG), mas ainda sem build nem teste da lib ([matriz](docs/PLANO.md#42-matriz-de-suporte)). |
| Links | Serial, TCP, BLE, ESP-NOW. **Um por boot**, mais o link peer opcional. |
| Dependências | [BTP](https://github.com/AlisonTristao/BTP) ≥ 2.51.0, [TinyShell](https://github.com/AlisonTristao/TinyShell) ≥ 2.2.0, `espressif/mdns`, `joltwallet/littlefs`, `espressif/esp_tinyusb` (S2/S3/P4) |

## Documentação

- [docs/PLANO.md](docs/PLANO.md): decisões, arquitetura, fases e testes pendentes.
- [CONTRIBUTING.md](CONTRIBUTING.md): organização do código e como contribuir.
- [CHANGELOG.md](CHANGELOG.md): histórico de versões.

## Licença

[MIT](LICENSE) © 2026 Alison de Oliveira Tristão
