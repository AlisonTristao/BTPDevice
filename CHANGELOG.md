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
