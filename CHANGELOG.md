# Changelog

Todas as mudanças relevantes deste projeto ficam registradas aqui.
O formato segue [Keep a Changelog](https://keepachangelog.com/pt-BR/1.1.0/) e o
versionamento segue [SemVer](https://semver.org/lang/pt-BR/).

## [Não lançado]

### Adicionado
- **Recovery e OTA** (Fase R, [PLANO 4.14](docs/PLANO.md#414-recovery-e-ota)): o app fica no
  `factory` e um firmware de recovery (`recovery/`, ~0,7 MB) na `ota_0`. `btp -recovery`,
  `btp -recovery_info`, `btp -factory_reset yes|all`, detector de crash loop
  (`DeviceConfig::recovery`), upload pela rede autenticado por HMAC com a chave E
  (`tools/btp_ota.py`, protocolo da aba OTA do TraceView), tabelas `partitions/btpdev_{4,8,16}mb.csv`,
  `tools/pio_recovery.py` e o exemplo `06_recovery_s3` (temperatura interna e LED RGB).
- Recovery: rede própria `btp-<nome>` com portal cativo quando nenhuma rede conhecida responde, e
  página em `/` para enviar o `.bin` pelo navegador, com a assinatura HMAC calculada na página
  (`CONFIG_BTPDEV_RECOVERY_AP`, `CONFIG_BTPDEV_RECOVERY_AP_PASSWORD`).
- **Economia de energia** (`DeviceConfig::power`, `PowerMode::Saving`): modem sleep do Wi-Fi, light
  sleep automático e CPU no cristal quando ocioso (com `CONFIG_PM_ENABLE` e
  `CONFIG_FREERTOS_USE_TICKLESS_IDLE`), sem desconectar. `btp -power`, `btp -power_set`,
  `BtpDevice::set_power_mode()`. No S3 com TCP: ~78% do tempo em light sleep, ping de 10 para 99 ms.
- **Sono profundo** (`btpdev/sleep.h`, `DeviceConfig::sleep`): `sleep_for()`, `sleep_until()`,
  `wake_reason()`, `wake_count()`, GPIO de despertar e de manter acordado, `btp -awake <min>` e
  `btp -sleep`. O `boot_id` vem de blocos reservados no NVS e segue pela memória do RTC: nenhuma
  escrita na flash por despertar. Exemplo `07_sleep_sensor`.
- **Hora** (`DeviceConfig::time`, `btpdev/time.h`): SNTP quando o link TCP tem IP, `btp -time`,
  `btp -time_set <hora>` (ISO 8601, hora local ou segundos desde 1970) e `btp -tz <POSIX>` (salvo
  no NVS; padrão Brasília, `<-03>3`). `BtpDevice::time_valid()`, `now()`, `now_us()`,
  `time_source()` e `set_time()`. A validade fica na memória do RTC: sobrevive a reboot e sono,
  não a falta de energia.
- **Configurações persistentes** (`App::settings()`, `btpdev/settings.h`): variáveis do app ligadas
  ao NVS com padrão, faixa, unidade, ajuda, `.reboot()` e `.on_change()`; carregadas antes de
  `commands()` e `topics()`. Módulo `cfg` (`-list`, `-get`, `-set`, `-reset`) e
  `BtpDevice::set_setting()`, `save_setting()`, `get_setting()`. O exemplo 06 guarda o pino do LED e o
  período de amostragem assim.
- Link Serial pela **USB-Serial-JTAG** (`SerialPort::UsbJtag`; o `Auto` a escolhe quando há um
  computador nela): a porta USB de gravação do S3, C3 e C6.
- `btp -bootloader`: reinicia no modo download da ROM para gravar por USB sem o botão BOOT;
  sem gravação, volta ao app em 30 s (watchdog do RTC), sem contar como crash.
- `tools/btp_term.py`: terminal BTP mínimo por Serial ou TCP, em claro ou cifrado com a chave E
  (`--password-e`), para testes de bancada sem o TraceView.
- `src/security/Sha256`: SHA-256 incremental e HMAC compartilhados (antes dentro do KeyStore).
- Estrutura inicial do repositório: licença MIT, README, CONTRIBUTING e o plano de
  implementação em [docs/PLANO.md](docs/PLANO.md).
- Componente ESP-IDF (`CMakeLists.txt`, `idf_component.yml`, `Kconfig`) que
  depende de BTP >= 2.49.0 e TinyShell >= 2.1.0.
- `BtpDevice` com `begin()` validando `DeviceConfig`/`StorageConfig`, códigos
  `BTPDEV_ERR_*`, `last_error()` e logs por tag (`btpdev.*`).
- Testes de host (`test/host`), `test_apps/build_smoke` e CI no GitHub Actions.
- Links Serial (USB CDC), TCP (Wi-Fi + mDNS, reconexão automática), BLE e ESP-NOW,
  com uma fila de envio de tamanho fixo e prioridade para respostas.
- Núcleo: identidade estável (MAC + contador de boot no NVS), nó BTP com sessão,
  manifesto (`source_info` com o nome do dispositivo) e STATUS, terminal do
  TraceView, shell em task própria e COMMAND_REQUEST (linha de shell / ping).
- `BtpDevice::state()`, `degraded()`, `routine()`/`tick()` (`Threading::Manual`),
  módulo de shell `btp` (`-status`, `-errors`) e queda automática para Serial.
- `DeviceConfig::wifi.ssid`/`password` para uma rede direto na config.
- Exemplo `examples/01_minimal` e `test_apps/link_echo` + `tools/link_echo_test.py`.
- Tópicos de telemetria (`btpdev::Topics`): campos ligados a variáveis ou
  `.fill()`, tópicos de texto, publicação só com assinatura e por evento
  (`BtpDevice::publish()`).
- Comandos (`btpdev::Commands`) registrados em `begin(app_commands, app_topics)`,
  `btpdev::printf()` para o terminal, `btp -topics` e `btp -reboot`.
- Exemplos `02_weather_station` (TCP) e `03_relay_actuator` (Serial).
- Armazenamento: `btpdev::FileSystem` com LittleFS e cartão SD (SPI/SDMMC),
  `BtpDevice::fs()`, redes extras no `wifi.txt`.
- Chave do dispositivo no NVS, importada do arquivo de `tools/provision_key.py`;
  cifragem AES-128-GCM pela chave E (links diretos) e E/L no ESP-NOW, conforme
  `bally_channels.h`.
- `Link::FromSettings` / `BtpDevice::set_link()` e os comandos `btp -key`,
  `-key_import`, `-key_forget`, `-link`, `-link_set`, `-wifi`, `-wifi_add`, `-fs`.
- Exemplos `04_ble_secure` e `05_espnow_dongle`; pasta `data/` com
  `pio run -t uploadfs` para provisionar.
- `btpdev::log()` / `log_record()`: linhas para o painel de logs do TraceView
  (mensagens LOG do BTP), de qualquer task.
- `BtpDevice::run()`: uma linha de shell vinda da aplicação (botão, agendador,
  script), com a saída no painel de logs.
- `BtpDevice::subscribed()`, `btpdev::in_command()`, `btpdev::identity()` e
  `BtpDevice::boot_id()`.
- `TopicDecl::range()` para qualquer tipo de campo e campos inteiros sem
  variável (preenchidos pelo `.fill()`).
- `StorageConfig::app_volume` (volume montado pela aplicação, como um SD
  compartilhado com USB Mass Storage) e `DeviceConfig::usb.install_driver`
  (USB composto instalado pela aplicação).
- `Commands::shell()` para registrar módulos que já existem em TinyShell.
- `DeviceConfig::serial_cleartext`: Serial sem cifra mesmo com chave (o rádio
  sempre segue a chave).
- Tópicos stream (`Topics::stream()` + `BtpDevice::write_stream()`): blocos de
  amostras `int16` (seq, taxa, amostras) enviados todos, em ordem, enquanto há
  assinatura; fila de blocos no dispositivo, contadores em `btp -topics`.
- Tópicos bytes (`Topics::bytes()`): corpo opaco (`OPAQUE_BYTES`, sem campos)
  que o receptor decodifica pelo nome e pela versão -- uma imagem, uma grade
  compactada. Publicado na taxa do assinante; a função devolve 0 quando não há
  nada novo, e o tópico fica pendente para a próxima passada. Pode passar de um
  frame (é fragmentado); `begin()` recusa um `max_size` maior que o buffer de
  selagem do preset de memória.
- `TopicDecl::version()`: versão de schema do tópico no manifesto e em cada
  amostra (campos, texto, stream e bytes); padrão 1.
- `btpdev::App` (`include/btpdev/app.h`): o dispositivo inteiro numa classe
  (`config()`, `storage()`, `commands()`, `topics()`), com o estado como membro
  e `device()` disponível dentro dela. `BtpDevice device(app); device.begin();`.
  A classe só é lida no `begin()`, então a ordem dos globais não importa. O
  exemplo 03 foi reescrito nesse estilo.
- `BtpDevice::CommandsFn`/`TopicsFn` passam a ser `std::function`: lambdas com
  captura funcionam no `begin()`. O `StorageConfig` do construtor ficou opcional.
- Handles de tópico (`btpdev::Topic`): toda declaração converte para um, e
  `publish()`, `subscribed()` e `write_stream()` aceitam o handle, sem busca
  pelo nome.
- `DeviceConfig::shell` (`ShellConfig`): núcleo, prioridade e pilha da task de
  comandos, que antes eram fixos (sem afinidade, 8 KB).
- `TaskConfig::timer`: um `esp_timer` periódico acorda a task de comunicação a
  cada `period_ms`, com período exato independente do `CONFIG_FREERTOS_HZ`. O
  timer só notifica a task.
- Hostname DHCP com o nome do dispositivo no link TCP: o roteador lista o
  dispositivo pelo nome, não como `espressif`.
- Link Serial por UART (`UartSerialLink`), para o conversor USB-serial da placa
  (CP210x, CH340): `DeviceConfig::serial_port` (`Auto`, `Usb`, `Uart`) e
  `DeviceConfig::uart` (porta, baud — padrão 921600 — pinos e `mute_console`).
  `Auto` usa a USB nativa quando o build tem TinyUSB CDC; senão, a UART. É o
  primeiro link do ESP32 clássico. Com a UART do console, o `ESP_LOG` fica mudo
  enquanto o link roda, para nenhuma linha de log cair no meio de um quadro.
- `btpdev::progress(label, percent)`: barra de progresso no terminal do
  TraceView, redesenhada na mesma linha. Aparece enquanto o comando ainda roda
  (a resposta sai abaixo dela) e funciona de qualquer task. Só o último valor
  é guardado (sem fila), no máximo ~10 redesenhos por segundo e 100% sempre
  enviado. A sobrecarga `(void*, uint8_t)` tem o formato de um callback C de
  progresso, então a função do trabalho não precisa incluir o BtpDevice: o
  registro do comando liga os dois. Exemplo em `01_minimal` (`demo -progress`).
  Usa `ShellLineEditor::writeProgress()` da TinyShell 2.2.0.
- `DeviceConfig::debug_log`: o `begin()` descreve o boot no console serial (só
  `ESP_LOG`, nunca o TraceView). Cada etapa sai com o tempo desde o boot e o heap
  que consumiu (identidade, armazenamento, chave, link, buffers, nó, shell,
  tasks), e no fim vem o heap livre, mínimo e o maior bloco, mais a stack usada e
  total de cada task (todas as do sistema com
  `CONFIG_FREERTOS_USE_TRACE_FACILITY`). Desligado por padrão.
- `btp -mem`: o mesmo relatório de memória e tasks, a qualquer hora, no terminal.

### Alterado
- **O suporte a cartão SD só entra no binário de quem usa:** `SdConfig::enabled` virou
  `SdConfig::driver` (`.sd = {.driver = btpdev::sd_card, ...}`). Sem cartão, o app do exemplo 06
  caiu de 1.198 para 1.084 KB (−114 KB de flash) e 10 KB de RAM estática.
- Stacks menores por padrão, pelo pico medido com manifesto, assinatura a 50 Hz e comandos ao mesmo
  tempo: `comms` 8 → 5 KB (pico 3,4 KB), `shell` 8 → 5 KB (2,8 KB), task do TCP 6 → 3 KB (1,3 KB). Um comando seu que precise de mais sobe
  `DeviceConfig::shell.stack_bytes`; o `btp -mem` mostra o pico de cada task.
- `DeviceConfig::psram` (`Off`, `Buffers`, `Stacks`): o que da lib vai para a PSRAM. `Buffers`
  (padrão) é o comportamento de antes; `Stacks` põe também as stacks da `comms` e das tasks de
  link. Sem `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP`, a lib lembra no log que o Wi-Fi também pode ir.
  As medidas por etapa do `debug_log` passam a contar só a RAM interna.
- `WifiConfig::buffers` (`Lean`, padrão, ou `Sdkconfig`): a lib reserva 6 buffers de recepção e
  usa transmissão dinâmica no `esp_wifi_init`. Com PSRAM ligada o IDF reservava 16 + 16 buffers
  estáticos (~51 KB) na RAM interna.
- **Resultado no ESP32-S3 N16R8 com link TCP:** a lib (com o Wi-Fi) passou de 124 para 53 KB de RAM
  interna (46 KB com `Psram::Stacks`), e o heap interno livre para o app de 168 para 258 KB.
- O aviso de stack curta do `begin()` mede a stack livre no ponto da chamada (a marca d'água
  contava também o que a inicialização do IDF usou antes do `app_main`).
- Requer TinyShell >= 2.2.0 (CI em `v2.2.0`) para `btpdev::progress()`. As
  mensagens da shell agora saem em inglês (`robot -pwm: success`,
  `nope: module not found`) e os argumentos digitados deixam de ser pretos
  (sumiam em fundo escuro).
- Requer BTP >= 2.51.0 (CI em `v2.51.0`): os presets de memória usam
  `btp::NodeWith<>` (capacidades por nome), e o `DeviceCore` passa pela
  verificação em compilação dos pares `has_X()`/`X()` do `NodeConfig`.
- `TopicDecl::f64()` não tem mais a forma ligada a uma variável: 8 bytes não
  são lidos de forma atômica pela task de comunicação no outro núcleo. Use a
  forma sem variável e `.fill()`.
- `Threading::Manual`: a primeira task que chama `routine()` ou `tick()` vira a
  dona do dispositivo, e chamadas de outra task são recusadas (e registradas
  uma vez no log), em vez de correr em paralelo sobre o nó BTP.

### Corrigido
- **Estouro de stack no `begin()` ao importar a chave:** montar o LittleFS e importar o `bally.key`
  passavam dos 3,5 KB padrão do `app_main` (crash loop no exemplo 06 depois de um `uploadfs`). O
  `begin()` agora roda numa task própria de 8 KB, temporária, no mesmo núcleo e prioridade de quem
  chamou; o `app_main` usa 1,2 KB. Saiu o aviso de stack curta, que não faz mais sentido.
- **Corrupção de heap no `begin()` com a stack padrão do `app_main` (3,5 KB):** o
  `btp::TopicBuilder` punha 32 `FieldRecord` (~2,5 KB) na stack de quem chama o `begin()`, e o
  estouro sobrescrevia o bloco do heap vizinho (assert no alocador bem depois, em outro lugar;
  no exemplo 06 virava crash loop). Os campos agora vão para o heap, e o `begin()` avisa quando a
  task chamadora tem menos de 2,5 KB de stack livre.
- Um S3 sem TinyUSB caía, sem chave, no link Serial da UART0 (o outro conector da DevKitC) e
  ficava inalcançável pela USB em uso: agora usa a USB-Serial-JTAG.
- A task de shell esperava o slot de conclusão girando em `vTaskDelay(1)` (10 ms
  por volta): agora a task de comunicação a notifica.
- A task de comunicação girava sem dormir com o tick padrão do IDF (100 Hz):
  esperas de 1 ms viravam 0 ticks (`ms_to_ticks()`).
- Frames maiores que 512 bytes eram recusados nos links Serial e TCP (o buffer
  COBS tinha tamanho fixo): agora segue o frame máximo do transporte, então um
  manifesto grande ou um texto de 1 KB também passam.
- `begin()` estourava a pilha de 3,5 KB do `app_main`: os helpers do catálogo
  não são mais inlinados em `TopicRegistry::install()`.
- Sem memória para a fila de um stream, `begin()` abortava (no ESP-IDF sem
  exceções, `new (std::nothrow)` chama `abort()`): a fila vem de
  `std::malloc` e a falta de memória vira `BTPDEV_ERR_MEMORY_BUDGET`.
