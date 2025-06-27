# Linux Usermode Driver Platform - Versão RISC-V

## Objetivo

Este repositório contém uma versão adaptada do módulo do kernel Linux que permite às aplicações em espaço de utilizador interagir diretamente com o hardware — de forma semelhante à abordagem utilizada em sistemas como o MINIX.

Este trabalho foi desenvolvido no âmbito do Projeto Integrador (PE39 - LEIC - FEUP).

---

## Autoria

O módulo UMDP foi originalmente criado por **Joaquim Monteiro**.  
Repositório original disponível em:  
[https://github.com/MonterraByte/linux-usermode-driver-platform](https://github.com/MonterraByte/linux-usermode-driver-platform/tree/master)

---

## Modificações Realizadas

As alterações nesta versão foram mínimas e concentraram-se nos ficheiros:

- `umdp_ac.c`
- `umdp_core.c`

Estas modificações foram necessárias para garantir o correto funcionamento do módulo em sistemas embebidos baseados em RISC-V.

---

## Requisitos

### Para Compilação

- Toolchain RISC-V: `gcc-riscv64-linux-gnu` (versão 10.5.0)
- Headers do Kernel Linux para RISC-V (normalmente incluídos no SDK do Milk-V Duo S)
- Ferramentas de compilação:
  - `make`
  - `meson`
  - `ninja`
- Dependências da biblioteca:
  - `libnl-3-dev`
  - `pkg-config`
- SDK do Milk-V Duo S: Para acesso aos headers corretos do kernel

### Para Execução no Milk-V Duo S

- Dispositivo Milk-V Duo S com Linux kernel 5.10 ou superior
- Ligação de rede (conexão USB) para SCP/SSH
- Acesso root no dispositivo
- Kernel compilado com suporte a módulos carregáveis (`CONFIG_MODULES=y`)

---

## Verificação dos Requisitos

Execute os seguintes comandos para confirmar se os requisitos estão instalados corretamente:

```bash
# Verificar a toolchain RISC-V
riscv64-linux-gnu-gcc-10 --version

# Verificar o Meson
meson --version

# Verificar se a libnl está disponível
pkg-config --exists libnl-3.0 && echo "libnl OK" || echo "libnl em falta"
```

--- 

## Compilação Simplificada

Para evitar configurações manuais complexas, estão incluídos scripts Bash que automatizam o processo de compilação.

## Compilar o Módulo do Kernel (`umdp.ko`)

```bash
cd umdp/
./build_with_gcc10.sh
```

Este script configura automaticamente o ambiente de cross-compilation e compila o módulo umdp.ko.

## Compilar a Biblioteca Estática

```bash
./build_with_gcc10_libnl_full_fixed.sh
```

Este script:

- Configura o ambiente de cross-compilation para RISC-V;
- Compila a biblioteca com suporte completo à `libnl;
- Gera uma biblioteca estática (`libumdp.a`) que pode ser facilmente integrada em projetos embebidos.

## Utilização

Após a compilação, o módulo pode ser carregado com o comando:

```bash
insmod umdp.ko
```

## Envio para o Milk-V Duo S

### Transferir o Módulo do Kernel

```bash
scp umdp/umdp.ko root@<IP_DO_MILK_V>:/tmp/
```

### Transferir a Biblioteca Estática

```bash
scp libumdp/build_riscv_libnl/libumdp.a root@<IP_DO_MILK_V>:/tmp/
```

Substituir `<IP_DO_MILK_V>` pelo endereço IP do seu dispositivo Milk-V Duo S.

### Aceder ao Dispositivo e Carregar o Módulo

```bash
ssh root@<IP_DO_MILK_V>
insmod /tmp/umdp.ko
```





