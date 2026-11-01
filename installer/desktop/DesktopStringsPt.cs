using System.Collections.Generic;

namespace SwitchWakerHD.Installer;

// Brazilian Portuguese for DesktopStrings.cs (Common), MacStrings.cs (Mac) and LinuxStrings.cs (Linux): the same keys.
static class DesktopStringsPt
{
    public static readonly Dictionary<string, string> Common = new()
    {
        ["py_desk_arch"]    = "Este computador ({0}) não é ARM64 nem x86-64.",
        ["py_desk_downloading"] = "Baixando o Python {0}...",
        ["py_desk_unpacking"] = "Preparando o Python...",
        ["py_desk_fail"]    = "O Python não inicia a partir de {0}. Apague essa pasta e clique em Tentar novamente.",
        ["sd_desk_todo"]    = "{0} arquivos para copiar ({1} já estavam no cartão).",
        ["sd_fail_desk"]    = "A cópia falhou ({0}): verifique se o cartão continua conectado e não está protegido contra gravação, e copie de novo; ou copie manualmente.",
        ["sd_ejected"]     = "Cartão ejetado: você já pode retirá-lo (ou sair do hekate).",
        ["sd_eject_fail"]  = "Não foi possível ejetar {0}: feche-o no gerenciador de arquivos e ejete-o por lá.",
        ["btn_eject"]      = "Ejetar cartão",
        ["btn_yes_cancel"] = "Sim, cancelar",
        ["btn_no"]         = "Não",
        ["mm_rvz_note"]    = "Um .rvz do Dolphin é lido com o Python 3.14, o mesmo que este instalador usa.",
        ["rel_no_script"]  = "A versão baixada não contém tools/switch/make_sd.py.",
        ["step_python"]    = "Python 3.14",
    };

    public static readonly Dictionary<string, string> Mac = new()
    {
        // Welcome
        ["welcome_body"]   = "Este assistente prepara o SwitchWakerHD a partir da sua própria cópia do jogo. Ele cuida de tudo: instala o que for preciso, gera o jogo para o Switch e o copia para o seu cartão SD. Não é preciso usar o Terminal.",
        ["welcome_li1"]    = "Python 3.14, só para o instalador, na pasta de trabalho dele (o Python do próprio Mac não é alterado)",
        ["welcome_li2"]    = "devkitPro, as ferramentas do Nintendo Switch (em /opt/devkitpro), e as ferramentas de linha de comando da Apple, se faltarem",
        ["welcome_admin"]  = "O macOS pedirá sua senha uma vez, para instalar o devkitPro. Na primeira vez, ele também pode pedir permissão para o instalador ler a pasta onde está o jogo: permita.",

        // Game page
        ["dlg_folder"]     = "Selecione a pasta do jogo (contém code, content e meta)",

        // Progress
        ["prog_body"]      = "Você pode deixar o Mac trabalhando (ligado na tomada, para que não entre em repouso). A geração do jogo leva de 10 a 30 minutos.",
        ["step_devkit"]    = "Ferramentas da Apple e devkitPro",


        // Apple's Command Line Tools
        ["clt_checking"]   = "Procurando as ferramentas de linha de comando da Apple...",
        ["clt_ok"]         = "Ferramentas de linha de comando da Apple prontas.",
        ["clt_notice_h"]   = "Precisamos da sua ajuda: ferramentas da Apple",
        ["clt_notice"]     = "O macOS abriu uma janela para instalar as ferramentas de linha de comando (make, git e o compilador).\n1. Clique em Instalar e aceite a licença.\n2. Espere terminar (alguns minutos).\n3. Clique em Continuar aqui.\nSe você não estiver vendo a janela, procure-a no Dock.",
        ["clt_missing"]    = "As ferramentas da Apple ainda não estão instaladas.",

        // devkitPro
        ["dkp_found_at"]   = "O devkitPro e os pacotes do Switch já estão em {0}.",
        ["dkp_mac_password"] = "Instalando o devkitPro e os pacotes do Switch: o macOS pede sua senha...",
        ["dkp_mac_prompt"] = "O instalador do SwitchWakerHD quer instalar o devkitPro (as ferramentas do Nintendo Switch) em /opt/devkitpro.",
        ["dkp_mac_cancelled"] = "A janela da senha foi cancelada: sem ela não é possível instalar o devkitPro. Clique em Tentar novamente e digite a senha do seu usuário do Mac.",
        ["dkp_mac_fail"]   = "Não foi possível pedir a senha para instalar o devkitPro ({0}). Clique em Tentar novamente; o registro tem os detalhes.",
        ["dkp_why_ssl"]    = "A conexão segura falhou (\"certificate has expired\"): verifique se a data e a hora do Mac estão corretas (Ajustes do Sistema > Geral > Data e Hora) e tente novamente.",
        ["dkp_why_lock"]   = "Outro processo do pacman está bloqueando o banco de dados. Espere ele terminar (ou reinicie o Mac) e tente novamente.",
        ["dkp_why_sig"]    = "O pacman não conseguiu verificar a assinatura dos pacotes. Clique em Tentar novamente; se falhar de novo, o registro tem o erro do pacman.",

        // Before anything
        ["pre_clock"]      = "A data e a hora do Mac ({0}) não batem com as da internet ({1}). Corrija-as (Ajustes do Sistema > Geral > Data e Hora): com o relógio errado, os downloads falham com \"certificate has expired\".",
        ["pre_tls"]        = "Não foi possível estabelecer uma conexão segura com o GitHub ({0}). Verifique a data e a hora do Mac, e se nenhum proxy ou filtro está interceptando as conexões HTTPS.",
        ["key_txt"]        = "{0} termina em .txt. No Finder, selecione-o, Arquivo > Obter Informações, e em Nome e Extensão deixe-o como {1}.",

        // Failures
        ["err_io"]         = "Erro de arquivos na etapa \"{0}\": {1}\nVerifique se há espaço livre no disco e se o macOS permite que o instalador leia a pasta do jogo (Ajustes do Sistema > Privacidade e Segurança > Arquivos e Pastas); depois clique em Tentar novamente.",
        ["hint_memory"]    = "O Mac ficou sem memória ao compilar. Feche outros programas e clique em Tentar novamente.",
        ["hint_tool_missing"] = "Falta uma ferramenta (do devkitPro ou da Apple). Clique em Tentar novamente: o instalador verifica tudo de novo.",
        ["hint_av"]        = "O macOS negou o acesso a um arquivo. Se o jogo estiver na Mesa, em Documentos, em Downloads ou em um disco externo, permita o acesso do instalador em Ajustes do Sistema > Privacidade e Segurança > Arquivos e Pastas (ou Acesso Total ao Disco) e clique em Tentar novamente.",
        ["hint_silent"]    = "A geração parou sem mensagem: uma ferramenta não chegou a iniciar. Clique em Tentar novamente; se acontecer de novo, compartilhe o registro (installer.log) em uma issue.",

        // Done page
        ["xfer_sd"]        = "Com o cartão microSD no Mac",
        ["sd_hint"]        = "Coloque o cartão microSD em um leitor de cartões. Não tem leitor? No Switch, abra hekate > Tools > USB Tools > SD Card e conecte o cabo USB: o cartão aparece no Finder e aqui.",
        ["sd_copied"]      = "Copiado para {0}. Ejete o cartão antes de retirá-lo.",
        ["sd_fat32"]       = "{0} está em FAT32, que não aceita arquivos com mais de 4 GB. Formate o cartão em exFAT (com o hekate ou o formatador oficial da SD Association).",
        ["manual_body"]    = "Copie o conteúdo desta pasta (a pasta switch) para a raiz do cartão microSD, substituindo os arquivos se perguntar. O Finder deixa arquivos ._ no cartão; a opção \"Com o cartão microSD no Mac\" não.",
        ["sd_eject_fail"]  = "Não foi possível ejetar {0}: feche-o no Finder e ejete-o por lá.",
        ["btn_open_out"]   = "Mostrar no Finder",
    };

    public static readonly Dictionary<string, string> Linux = new()
    {
        // Welcome
        ["welcome_li1"]    = "Python 3.14, só para o instalador, na pasta de trabalho dele (o Python do sistema não é alterado)",
        ["welcome_li2"]    = "devkitPro, as ferramentas do Nintendo Switch (em /opt/devkitpro), ou Docker/Podman se você já tiver",
        ["welcome_admin"]  = "Se for preciso instalar algo no sistema, ele pedirá sua senha.",

        // Toolchain
        ["lx_tc_native"]   = "devkitPro encontrado em {0}; compilando sem contêiner.",
        ["lx_tc_container"]= "Compilando com {0} (a imagem devkitpro/devkita64; baixada na primeira vez, ~1,5 GB).",
        ["lx_tc_pkgs"]     = "Faltam pacotes no devkitPro: {0}",
        ["lx_tc_hosttools"]= "Faltam ferramentas do sistema: {0}",
        ["lx_tc_docker_denied"] = "{0} está instalado, mas o seu usuário não pode usá-lo (no Docker, adicione-o ao grupo docker e entre na sessão de novo).",
        ["lx_tc_what_dkp"] = "o devkitPro e os pacotes do Switch dele",
        ["lx_tc_what_podman"] = "o Podman (para compilar no contêiner do devkitPro)",
        ["lx_tc_unknown"]  = "Sua distribuição não foi reconhecida. Instale o Docker ou o Podman com o seu gerenciador de pacotes e clique em Continuar.",
        ["lx_tc_not_found"]= "Ainda não há com o que compilar: nem o devkitPro com seus pacotes, nem um Docker/Podman utilizável. Veja os detalhes.",
        ["lx_fwd_container"] = "O ícone do menu HOME só é criado com o devkitPro instalado (não no contêiner): desta vez o jogo é gerado sem ele.",
        ["lx_root_h"]      = "Precisamos da sua senha",
        ["lx_root_body"]   = "Para instalar {0}, é preciso executar uma etapa como administrador. Abra um terminal (Ctrl+Alt+T), cole este comando, digite sua senha e, quando terminar, clique em Continuar.",
        ["lx_root_pkexec"] = "Instalando {0}: digite sua senha na janela que se abrir...",
        ["lx_root_direct"] = "Instalando {0} (o instalador já está rodando como administrador)...",
        ["lx_root_failed"] = "A etapa de administrador não terminou (código {0}).",
        ["btn_copy_cmd"]   = "Copiar comando",
        ["btn_done_continue"] = "Pronto, continuar",
        ["dkp_found_at"]   = "devkitPro encontrado: {0}",
        ["dkp_why_ssl"]    = "A conexão segura falhou (\"certificate has expired\"): verifique se a data e a hora do sistema estão corretas e tente novamente.",
        ["dkp_why_lock"]   = "Outro processo do pacman está bloqueando o banco de dados. Espere ele terminar e tente novamente.",
        ["dkp_why_sig"]    = "O pacman não conseguiu verificar a assinatura dos pacotes. Clique em Tentar novamente; se falhar de novo, o registro tem o erro do pacman.",

        // Before anything
        ["pre_clock"]      = "A data e a hora do sistema ({0}) não batem com as da internet ({1}). Corrija-as (ative a hora automática): com o relógio errado, os downloads falham com \"certificate has expired\".",
        ["pre_tls"]        = "Não foi possível estabelecer uma conexão segura com o GitHub ({0}). Verifique a data e a hora do sistema, e se nenhum proxy está interceptando as conexões HTTPS.",
        ["key_txt"]        = "{0} termina em .txt: renomeie-o para {1}.",

        // Failures
        ["err_io"]         = "Erro de arquivos na etapa \"{0}\": {1}\nVerifique se há espaço livre no disco e se o seu usuário pode ler a pasta do jogo; depois clique em Tentar novamente.",
        ["hint_memory"]    = "O computador ficou sem memória ao compilar. Feche outros programas e clique em Tentar novamente.",
        ["hint_tool_missing"] = "Falta uma ferramenta (do devkitPro ou do sistema). Clique em Tentar novamente: o instalador verifica tudo de novo.",
        ["hint_av"]        = "O acesso a um arquivo foi negado: verifique se o seu usuário pode ler a pasta do jogo e gravar em ~/SwitchWakerHD-Build, e clique em Tentar novamente.",
        ["hint_silent"]    = "A geração parou sem mensagem: uma ferramenta não chegou a iniciar. Clique em Tentar novamente; se acontecer de novo, compartilhe o registro (installer.log) em uma issue.",

        // Done page
        ["sd_hint"]        = "Coloque o cartão microSD em um leitor de cartões. Não tem leitor? No Switch, abra hekate > Tools > USB Tools > SD Card e conecte o cabo USB: o cartão aparece aqui como uma unidade. Se não aparecer, abra-o no gerenciador de arquivos para montá-lo.",
        ["sd_fat32"]       = "{0} está em FAT32, que não aceita arquivos com mais de 4 GB. Formate o cartão em exFAT (com o hekate).",
    };
}
