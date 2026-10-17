---
alwaysApply: true
---

# Arquitetura
- Linguagem de programação: C++ voltada ao ESP32
- Consumo de API via HTTP
- Armazenamento em Preferences do que precisar salvar mesmo com a placa desligada
- Link da documentação da API: https://portguard.milesmp.com.br/docs/api (usar como referência ao ter implementações que envolvem requisições HTTP)

# Padrões de código
- Toda função precisa de um comentário explicando o que ela faz. 
- Toda variável e método precisa ser em português
- Toda variável e método precisa ter o nome extenso, sem abreviações, onde funções são no padrão camelCase e variáveis/parâmetros em snake_case:
  - CERTO: conectaWifi(), salvaEstadoInternet(), verificaConexão(), senha_wifi, variavel_exemplo
  - ERRADO: conWf(), setNetSt(), vc(), vr_ex, tc_2
  - use nomes declarativos e informativos, não importando se vai ficar extenso
  - funções de testes SEMPRE começam com teste_ para melhorar identificação. Exemplo: teste_conexao_wifi
- Não fragmente lógicas! Se a função conectaWifi precisa ter lógicas distintas, mantenha tudo na mesma função! Somente caso alguma nova função use algo que está em conectaWifi que a lógica específica será desacoplada. Não crie diversos métodos/funções para algo centralizado!
- Todo output de texto / log deve usar a função print_log para manter padrão de exibição de logs

# Padrões de entrega
- Ao entregar uma funcionalidade, garanta que tudo está funcionando para garantir que a implementação não gerou impacto
- Garanta que os padrões de código foram seguidos na implementação
- Ao implementar uma nova funcionalidade, crie o arquivo .cursor/contexts/business-rule-{regra implementada}.md para recuperação de contexto no futuro.
- CRUCIAL: Toda implementação precisa obrigatóriamente ter uma função de teste e chamar ela dentro da função teste_completo. O teste_completo() será executado ao final do setup para garantir que todas as implementações continuam funcionando após novas implementações. SEMPRE implemente testes, independente do que é implementado e ao finalizar, execute os testes e caso ele retorne false, reveja o que pode estar dando problema. As funções de testes sempre devem retornar true em caso de sucesso (lógica funcionando normalmente) e false em caso de problemas. Toda chamada de funções de testes dentro de teste_completo() deve estar dentro de ifs para caso um teste retorne false, a função retorne false também e exiba o motivo via print_log.

