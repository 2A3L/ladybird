/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Platform.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibWebCommon/Page/CredentialFill.h>
#include <UI/Qt/BrowserWindow.h>
#include <UI/Qt/HubConnection.h>
#include <UI/Qt/KeePassXC.h>
#include <UI/Qt/StringUtils.h>
#include <UI/Qt/Tab.h>
#include <UI/Qt/WebContentView.h>

#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFormLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QMessageBox>
#include <QProcess>
#include <QProgressDialog>
#include <QTimer>

namespace Ladybird {

#if defined(AK_OS_LINUX) && defined(LADYBIRD_HUB_IN_OWN_PROCESS)

struct KeePassContext {
    u64 view_id;
    Web::HTML::CrossProcessId document_id;
    String origin;
    ByteString profile;

    bool still_current(BrowserWindow& window) const
    {
        auto* tab = window.current_tab();
        if (!tab || !tab->is_loaded() || tab->view().view_id() != view_id)
            return false;
        auto current = HubConnection::the().view_keepass_context(view_id);
        return current.document_id().has_value() && *current.document_id() == document_id && current.origin() == origin;
    }
};

static void notify(BrowserWindow& window, QString const& message)
{
    auto* box = new QMessageBox(QMessageBox::Information, QStringLiteral("KeePassXC"), message, QMessageBox::Ok, &window);
    box->setTextFormat(Qt::PlainText);
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->open();
}

static void stop_keepassxc_helper(QProcess& process)
{
    process.terminate();
    QTimer::singleShot(1000, &process, [&process] {
        if (process.state() != QProcess::NotRunning)
            process.kill();
    });
}

static void call_keepassxc(BrowserWindow& window, KeePassContext const& context, QJsonObject request, Function<void(QJsonValue)> on_result)
{
    if (window.findChild<QProcess*>(QStringLiteral("KeePassXCRequest"), Qt::FindDirectChildrenOnly))
        return;

    QFile bridge(QStringLiteral(":/keepassxc_bridge.py"));
    if (!bridge.open(QIODevice::ReadOnly)) {
        notify(window, QObject::tr("Could not load KeePassXC support."));
        return;
    }
    request.insert(QStringLiteral("origin"), qstring_from_ak_string(context.origin));
    request.insert(QStringLiteral("profile"), QFile::decodeName(context.profile.characters()));
    request.insert(QStringLiteral("unlock"), true);

    auto* process = new QProcess(&window);
    process->setObjectName(QStringLiteral("KeePassXCRequest"));
    process->setProgram(QStringLiteral("/usr/bin/python3"));
    process->setArguments({ QStringLiteral("-I"), QStringLiteral("-c"), QString::fromUtf8(bridge.readAll()) });
    process->setStandardErrorFile(QProcess::nullDevice());

    auto* progress = new QProgressDialog(QObject::tr("Waiting for KeePassXC…"), QObject::tr("Cancel"), 0, 0, &window);
    progress->setWindowModality(Qt::WindowModal);
    progress->setWindowTitle(QStringLiteral("KeePassXC"));
    progress->setAutoClose(false);
    progress->setAutoReset(false);
    progress->setMinimumDuration(400);

    QObject::connect(progress, &QProgressDialog::canceled, process, [process] {
        process->setProperty("cancelled", true);
        stop_keepassxc_helper(*process);
    });
    QObject::connect(process, &QProcess::readyReadStandardOutput, process, [process] {
        if (process->bytesAvailable() > static_cast<qint64>(Web::maximum_credential_fill_payload_size))
            stop_keepassxc_helper(*process);
    });
    QObject::connect(process, &QProcess::errorOccurred, &window, [&window, process, progress](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart)
            return;
        progress->deleteLater();
        process->deleteLater();
        notify(window, QObject::tr("Could not start the KeePassXC helper."));
    });
    QObject::connect(process, &QProcess::finished, &window, [&window, process, progress, on_result = move(on_result)](int exit_code, QProcess::ExitStatus exit_status) mutable {
        progress->hide();
        progress->deleteLater();
        process->setObjectName(QString());
        process->deleteLater();
        if (process->property("cancelled").toBool())
            return;
        auto output = process->readAllStandardOutput();
        if (exit_status != QProcess::NormalExit || exit_code != 0 || output.size() > static_cast<qsizetype>(Web::maximum_credential_fill_payload_size)) {
            notify(window, QObject::tr("The KeePassXC request failed."));
            return;
        }
        auto response = QJsonDocument::fromJson(output).object();
        output.fill('\0');
        if (auto error = response.value(QStringLiteral("error")).toString(); !error.isEmpty()) {
            notify(window, error);
            return;
        }
        if (!response.contains(QStringLiteral("value"))) {
            notify(window, QObject::tr("The KeePassXC request failed."));
            return;
        }
        on_result(response.value(QStringLiteral("value")));
    });
    QObject::connect(process, &QProcess::started, process, [process, request = QJsonDocument(request).toJson(QJsonDocument::Compact)]() mutable {
        process->write(request);
        process->closeWriteChannel();
        request.fill('\0');
    });
    QTimer::singleShot(125000, process, [process] { stop_keepassxc_helper(*process); });
    process->start();
    progress->setValue(0);
}

static QJsonObject choose_account(BrowserWindow& window, KeePassContext const& context, QJsonArray const& accounts)
{
    if (accounts.isEmpty()) {
        notify(window, QObject::tr("KeePassXC has no accounts for this site."));
        return {};
    }
    QStringList options;
    for (qsizetype index = 0; index < accounts.size(); ++index) {
        auto account = accounts[index].toObject();
        options.append(QStringLiteral("%1. %2 (%3)").arg(index + 1).arg(account.value(QStringLiteral("name")).toString(), account.value(QStringLiteral("login")).toString()));
    }
    bool chosen = false;
    auto option = QInputDialog::getItem(&window, QStringLiteral("KeePassXC"), QObject::tr("Account for %1").arg(qstring_from_ak_string(context.origin)), options, 0, false, &chosen);
    auto index = options.indexOf(option);
    return chosen && index >= 0 ? accounts[index].toObject() : QJsonObject {};
}

static void fill_from_keepassxc(BrowserWindow& window, KeePassContext const& context, QJsonObject const& account)
{
    auto uuid = account.value(QStringLiteral("uuid")).toString();
    auto login = account.value(QStringLiteral("login")).toString();
    if (uuid.isEmpty() || !context.still_current(window))
        return;
    call_keepassxc(window, context, { { QStringLiteral("op"), QStringLiteral("fill") }, { QStringLiteral("uuid"), uuid } },
        [&window, context, login](QJsonValue value) {
            // Focus may return to the browser after the unlock progress dialog disappears.
            QTimer::singleShot(0, &window, [&window, context, login, value = move(value)] {
                if (!context.still_current(window) || !window.isActiveWindow())
                    return;
                auto credentials = value.toObject();
                if (credentials.value(QStringLiteral("login")).toString() != login || credentials.value(QStringLiteral("password")).toString().isEmpty()) {
                    notify(window, QObject::tr("The KeePassXC account changed. Choose the account again."));
                    return;
                }
                auto payload = QJsonDocument(credentials).toJson(QJsonDocument::Compact);
                if (payload.size() > static_cast<qsizetype>(Web::maximum_credential_fill_payload_size)) {
                    notify(window, QObject::tr("The KeePassXC response is too large."));
                    return;
                }
                auto buffer = Core::AnonymousBuffer::create_with_size(static_cast<size_t>(payload.size()));
                if (buffer.is_error()) {
                    notify(window, QObject::tr("Could not prepare the KeePassXC response."));
                    return;
                }
                memcpy(buffer.value().data<char>(), payload.constData(), static_cast<size_t>(payload.size()));
                payload.fill('\0');

                auto& view = window.current_tab()->view();
                view.setFocus(Qt::ShortcutFocusReason);
                if (!view.hasFocus())
                    return;

                // Publish focus before filling, and keep secrets outside message bytes used by IPC diagnostics.
                view.set_has_system_focus(true);
                HubConnection::the().async_view_fill_keepass_credentials(context.view_id, context.document_id, context.origin, buffer.release_value());
            });
        });
}

static void store_in_keepassxc(BrowserWindow& window, KeePassContext const& context, QJsonObject const& account)
{
    auto uuid = account.value(QStringLiteral("uuid")).toString();
    QDialog dialog(&window);
    dialog.setWindowTitle(uuid.isEmpty() ? QObject::tr("Save in KeePassXC") : QObject::tr("Update in KeePassXC"));
    QFormLayout layout(&dialog);
    QLineEdit site(qstring_from_ak_string(context.origin), &dialog);
    site.setReadOnly(true);
    QLineEdit login(account.value(QStringLiteral("login")).toString(), &dialog);
    QLineEdit password(&dialog);
    password.setEchoMode(QLineEdit::Password);
    QDialogButtonBox buttons(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    layout.addRow(QObject::tr("Site"), &site);
    layout.addRow(QObject::tr("Username"), &login);
    layout.addRow(QObject::tr("Password"), &password);
    layout.addRow(&buttons);
    QObject::connect(&buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(&buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted || !context.still_current(window))
        return;
    if (password.text().isEmpty()) {
        notify(window, QObject::tr("Enter a password to save."));
        return;
    }
    call_keepassxc(window, context,
        { { QStringLiteral("op"), uuid.isEmpty() ? QStringLiteral("new") : QStringLiteral("update") },
            { QStringLiteral("uuid"), uuid },
            { QStringLiteral("login"), login.text() },
            { QStringLiteral("password"), password.text() } },
        [&window](QJsonValue result) {
            notify(window, result.toBool() ? QObject::tr("Saved to KeePassXC.") : QObject::tr("KeePassXC did not confirm the save."));
        });
}

void request_keepassxc(BrowserWindow& window, bool manage_accounts)
{
    auto* tab = window.current_tab();
    if (!tab || !tab->is_loaded())
        return;
    auto id = tab->view().view_id();
    auto response = HubConnection::the().view_keepass_context(id);
    if (!response.document_id().has_value()) {
        notify(window, QObject::tr("Choose a regular HTTPS tab to use KeePassXC."));
        return;
    }
    KeePassContext context { id, *response.document_id(), response.take_origin(), response.take_profile() };

    if (!manage_accounts) {
        call_keepassxc(window, context, { { QStringLiteral("op"), QStringLiteral("list") } }, [&window, context](QJsonValue value) {
            if (context.still_current(window)) {
                auto account = choose_account(window, context, value.toArray());
                if (!account.isEmpty())
                    fill_from_keepassxc(window, context, account);
            }
        });
        return;
    }

    QMessageBox choice(&window);
    choice.setWindowTitle(QStringLiteral("KeePassXC"));
    choice.setTextFormat(Qt::PlainText);
    choice.setText(qstring_from_ak_string(context.origin));
    auto* new_entry = choice.addButton(QObject::tr("Save New…"), QMessageBox::AcceptRole);
    auto* update_entry = choice.addButton(QObject::tr("Update…"), QMessageBox::ActionRole);
    choice.addButton(QMessageBox::Cancel);
    choice.exec();
    if (!context.still_current(window))
        return;
    if (choice.clickedButton() == new_entry) {
        store_in_keepassxc(window, context, {});
    } else if (choice.clickedButton() == update_entry) {
        call_keepassxc(window, context, { { QStringLiteral("op"), QStringLiteral("list") } }, [&window, context](QJsonValue value) {
            if (context.still_current(window)) {
                auto account = choose_account(window, context, value.toArray());
                if (!account.isEmpty())
                    store_in_keepassxc(window, context, account);
            }
        });
    }
}

#else

void request_keepassxc(BrowserWindow&, bool)
{
}

#endif

}
