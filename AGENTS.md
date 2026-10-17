# FIRMWARE PARA PLACA ESP32 DA PORTGUARD
Placa para monitoramento de abertura indevida e gestão de portões

## Fluxo de carregamento
Sempre ao receber um prompt, carregue:
1. .cursor/contexts/architecture.md -> entendimento da arquitetura do projeto
2. .cursor/rules/implementation.md -> regras de implementação
3. .cursor/contexts/business-rule-general.md -> entendimento da regra de negócio geral
3. .cursor/contexts/business-rule-*.md -> entendimento da regra de negócio solicitada no prompt (Caso tenha o arquivo de regra de negócio)

Todo código deve ser escrito somente no arquivo script.cpp.