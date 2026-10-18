using System.Collections.Generic;

namespace SwitchWakerHD.Installer;

// The wording the macOS and Linux installers share, then each system's (MacStrings.cs, LinuxStrings.cs): these keys
// win over shared/Strings.cs (Str.Override).
static class DesktopStrings
{
    public static Dictionary<string, (string es, string en)> For(bool mac)
    {
        var table = new Dictionary<string, (string es, string en)>(Common);
        foreach (var kv in mac ? MacStrings.Table : LinuxStrings.Table) table[kv.Key] = kv.Value;
        return table;
    }

    static readonly Dictionary<string, (string es, string en)> Common = new()
    {
        ["py_desk_arch"]    = ("Este equipo ({0}) no es ARM64 ni x86-64.", "This computer ({0}) is neither ARM64 nor x86-64."),
        ["py_desk_downloading"] = ("Descargando Python {0}...",                        "Downloading Python {0}..."),
        ["py_desk_unpacking"] = ("Preparando Python...",                               "Setting up Python..."),
        ["py_desk_fail"]    = ("Python no arranca desde {0}. Borra esa carpeta y pulsa Reintentar.", "Python does not start from {0}. Delete that folder and press Retry."),
        ["sd_desk_todo"]    = ("{0} archivos que copiar ({1} ya estaban en la tarjeta).", "{0} files to copy ({1} were already on the card)."),
        ["sd_fail_desk"]    = ("La copia falló ({0}): comprueba que la tarjeta sigue conectada y no está protegida contra escritura, y vuelve a copiar; o cópiala a mano.",
                               "The copy failed ({0}): check that the card is still connected and not write-protected, and copy again; or copy it by hand."),
        ["sd_ejected"]     = ("Tarjeta expulsada: ya puedes sacarla (o salir de hekate).", "Card ejected: you can remove it now (or leave hekate)."),
        ["sd_eject_fail"]  = ("No se pudo expulsar {0}: ciérrala en el gestor de archivos y expúlsala desde allí.", "Could not eject {0}: close it in the file manager and eject it there."),
        ["btn_eject"]      = ("Expulsar tarjeta",                                     "Eject card"),
        ["btn_yes_cancel"] = ("Sí, cancelar",                                         "Yes, cancel"),
        ["btn_no"]         = ("No",                                                   "No"),
        ["mm_rvz_note"]    = ("Un .rvz de Dolphin se lee con Python 3.14, que es el que usa este instalador.",
                               "A Dolphin .rvz is read with Python 3.14, the one this installer uses."),
        ["rel_no_script"]  = ("La versión descargada no contiene tools/switch/make_sd.py.", "The downloaded release has no tools/switch/make_sd.py."),
        ["step_python"]    = ("Python 3.14",                                          "Python 3.14"),
    };
}
