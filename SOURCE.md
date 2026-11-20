# Proveniência

- Origem: subdiretório `esp/AUDIOLINK` do projeto CumeStack.
- Ramo de desenvolvimento: `lab/audiolink-v2btn`.
- Commit de origem: `871f2c91f47c7d27eb66eee1a64e174ecf9b516d`.
- Publicação: 20 de novembro de 2026.
- Fontes C/H copiados sem alterações; seleção limitada ao AudioLink.
- Ajustes de publicação: scripts de build portáveis por `IDF_PATH`, documentação de entrada e exclusão de artefactos de bancada. O helper V2P inclui apenas o alvo RX presente nesta publicação.
- A palavra-passe SoftAP original foi removida do Kconfig e da documentação; o valor por omissão está vazio e tem de ser configurado localmente.
- O runner de testes ativa as constantes matemáticas da libc em Linux e liga explicitamente libm; os testes e o firmware C/H permanecem idênticos à origem.
