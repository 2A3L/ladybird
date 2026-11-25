/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/StringView.h>
#include <QString>
#include <UI/Qt/Actions.h>

namespace Ladybird {

void set_interface_language(StringView);
QString localized_action_text(ActionID, StringView);
QString localized_menu_title(StringView);

}
