/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Qt/Localization.h>
#include <UI/Qt/StringUtils.h>

namespace Ladybird {

static bool s_spanish;

void set_interface_language(StringView language)
{
    s_spanish = language == "es"sv;
}

QString localized_action_text(ActionID id, StringView text)
{
    if (s_spanish) {
        // Match both the action and its source text, so user-provided text is never translated.
#define TRANSLATE_ACTION(action_id, source, translation) \
    if (id == ActionID::action_id && text == source##sv) \
    return QStringLiteral(translation)
        TRANSLATE_ACTION(NavigateBack, "Back", "Atrás");
        TRANSLATE_ACTION(NavigateForward, "Forward", "Adelante");
        TRANSLATE_ACTION(Reload, "Reload", "Recargar");
        TRANSLATE_ACTION(ReloadBypassingCache, "Reload Without Cache", "Recargar sin caché");
        TRANSLATE_ACTION(Undo, "Undo", "Deshacer");
        TRANSLATE_ACTION(Redo, "Redo", "Rehacer");
        TRANSLATE_ACTION(CopySelection, "Copy", "Copiar");
        TRANSLATE_ACTION(CutSelection, "Cut", "Cortar");
        TRANSLATE_ACTION(Paste, "Paste", "Pegar");
        TRANSLATE_ACTION(PasteAndMatchStyle, "Paste and Match Style", "Pegar y adaptar al estilo");
        TRANSLATE_ACTION(SelectAll, "Select All", "Seleccionar todo");
        TRANSLATE_ACTION(OpenAboutPage, "About Ladybird", "Acerca de Ladybird");
        TRANSLATE_ACTION(ViewDownloads, "Downloads", "Descargas");
        TRANSLATE_ACTION(OpenSettingsPage, "Settings", "Configuración");
        TRANSLATE_ACTION(ViewHistory, "View History", "Ver historial");
        TRANSLATE_ACTION(ClearBrowsingData, "Clear Browsing Data", "Borrar datos de navegación");
        TRANSLATE_ACTION(ZoomIn, "Zoom In", "Acercar");
        TRANSLATE_ACTION(ZoomOut, "Zoom Out", "Alejar");
        TRANSLATE_ACTION(ResetZoom, "Reset Zoom", "Restablecer zoom");
        TRANSLATE_ACTION(ToggleVerticalTabsExpanded, "Show Sidebar", "Mostrar barra lateral");
        TRANSLATE_ACTION(ToggleVerticalTabsExpanded, "Hide Sidebar", "Ocultar barra lateral");
        TRANSLATE_ACTION(ToggleMenuBar, "Show Menu Bar", "Mostrar barra de menús");
        TRANSLATE_ACTION(ViewSource, "View Page Source", "Ver código fuente de la página");
#undef TRANSLATE_ACTION
    }
    return qstring_from_ak_string(text);
}

QString localized_menu_title(StringView title)
{
    if (s_spanish) {
        if (title == "&File"sv)
            return QStringLiteral("&Archivo");
        if (title == "&Edit"sv)
            return QStringLiteral("&Editar");
        if (title == "&View"sv)
            return QStringLiteral("&Ver");
        if (title == "&Inspect"sv)
            return QStringLiteral("&Inspeccionar");
        if (title == "&Translation"sv)
            return QStringLiteral("&Traducción");
        if (title == "Bookmarks"sv)
            return QStringLiteral("Marcadores");
        if (title == "History"sv)
            return QStringLiteral("Historial");
        if (title == "Inspect"sv)
            return QStringLiteral("Inspeccionar");
        if (title == "Debug"sv)
            return QStringLiteral("Depurar");
        if (title == "Help"sv)
            return QStringLiteral("Ayuda");
    }
    return qstring_from_ak_string(title);
}

}
