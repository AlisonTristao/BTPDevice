# Changelog

Todas as mudanças relevantes deste projeto ficam registradas aqui.
O formato segue [Keep a Changelog](https://keepachangelog.com/pt-BR/1.1.0/) e o
versionamento segue [SemVer](https://semver.org/lang/pt-BR/).

## [Não lançado]

### Adicionado
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

### Alterado
- `TopicDecl::f64()` não tem mais a forma ligada a uma variável: 8 bytes não
  são lidos de forma atômica pela task de comunicação no outro núcleo. Use a
  forma sem variável e `.fill()`.
- `Threading::Manual`: a primeira task que chama `routine()` ou `tick()` vira a
  dona do dispositivo, e chamadas de outra task são recusadas (e registradas
  uma vez no log), em vez de correr em paralelo sobre o nó BTP.

### Corrigido
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
