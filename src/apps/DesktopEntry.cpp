#include "apps/DesktopEntry.h"

#include <QHash>
#include <QStringView>

#include <algorithm>

namespace quantum::apps {

namespace {

// The value escapes the specification defines for `string` values: `\s` space, `\n`, `\t`, `\r`, `\\`. Any
// other backslash sequence is left as written: the specification does not define it, and dropping the
// backslash would change what the file said.
QString unescapeValue(const QString& raw)
{
    QString out;
    out.reserve(raw.size());
    for (qsizetype i = 0; i < raw.size(); ++i) {
        const QChar c = raw.at(i);
        if (c != QLatin1Char('\\') || i + 1 >= raw.size()) {
            out.append(c);
            continue;
        }
        const QChar next = raw.at(i + 1);
        switch (next.unicode()) {
        case 's': out.append(QLatin1Char(' ')); ++i; break;
        case 'n': out.append(QLatin1Char('\n')); ++i; break;
        case 't': out.append(QLatin1Char('\t')); ++i; break;
        case 'r': out.append(QLatin1Char('\r')); ++i; break;
        case '\\': out.append(QLatin1Char('\\')); ++i; break;
        default: out.append(c); break;
        }
    }
    return out;
}

// `a;b;c` with `\;` as a literal semicolon, as the specification's list values are written. A trailing
// separator does not make an empty last element.
QStringList splitList(const QString& value)
{
    QStringList items;
    QString current;
    for (qsizetype i = 0; i < value.size(); ++i) {
        const QChar c = value.at(i);
        if (c == QLatin1Char('\\') && i + 1 < value.size() && value.at(i + 1) == QLatin1Char(';')) {
            current.append(QLatin1Char(';'));
            ++i;
        } else if (c == QLatin1Char(';')) {
            items.append(current);
            current.clear();
        } else {
            current.append(c);
        }
    }
    if (!current.isEmpty())
        items.append(current);
    items.removeAll(QString());
    return items;
}

// The locale names a `Name[...]` key may carry, best first, from a POSIX locale `lang_COUNTRY.ENCODING@MODIFIER`.
// The specification's order: lang_COUNTRY@MODIFIER, lang_COUNTRY, lang@MODIFIER, lang.
QStringList localeCandidates(const QString& locale)
{
    QString rest = locale;
    QString modifier;
    const qsizetype at = rest.indexOf(QLatin1Char('@'));
    if (at >= 0) {
        modifier = rest.mid(at);
        rest.truncate(at);
    }
    const qsizetype dot = rest.indexOf(QLatin1Char('.'));
    if (dot >= 0)
        rest.truncate(dot);
    QString country;
    const qsizetype underscore = rest.indexOf(QLatin1Char('_'));
    if (underscore >= 0) {
        country = rest.mid(underscore);
        rest.truncate(underscore);
    }
    if (rest.isEmpty() || rest == QLatin1String("C") || rest == QLatin1String("POSIX"))
        return {};

    QStringList candidates;
    if (!country.isEmpty() && !modifier.isEmpty())
        candidates << rest + country + modifier;
    if (!country.isEmpty())
        candidates << rest + country;
    if (!modifier.isEmpty())
        candidates << rest + modifier;
    candidates << rest;
    return candidates;
}

}  // namespace

std::optional<QStringList> splitExec(const QString& exec, const QString& name)
{
    QStringList arguments;
    QString current;
    // Whether the argument being built has contributed anything that is not a removed field code: an argument
    // that was only `%f` disappears, one that was `""` is a real empty argument.
    bool present = false;
    bool inQuotes = false;

    const auto finish = [&] {
        if (present)
            arguments.append(current);
        current.clear();
        present = false;
    };

    for (qsizetype i = 0; i < exec.size(); ++i) {
        const QChar c = exec.at(i);
        if (inQuotes) {
            if (c == QLatin1Char('"')) {
                inQuotes = false;
                continue;
            }
            if (c == QLatin1Char('\\') && i + 1 < exec.size()) {
                const QChar next = exec.at(i + 1);
                if (next == QLatin1Char('"') || next == QLatin1Char('`') || next == QLatin1Char('$')
                    || next == QLatin1Char('\\')) {
                    current.append(next);
                    present = true;
                    ++i;
                    continue;
                }
            }
            if (c == QLatin1Char('%') && i + 1 < exec.size()) {
                const QChar code = exec.at(i + 1);
                ++i;
                if (code == QLatin1Char('%')) {
                    current.append(QLatin1Char('%'));
                    present = true;
                } else if (code == QLatin1Char('c')) {
                    current.append(name);
                    present = true;
                }
                continue;
            }
            current.append(c);
            present = true;
            continue;
        }

        if (c == QLatin1Char('"')) {
            inQuotes = true;
            present = true;
        } else if (c.isSpace()) {
            finish();
        } else if (c == QLatin1Char('%') && i + 1 < exec.size()) {
            const QChar code = exec.at(i + 1);
            ++i;
            if (code == QLatin1Char('%')) {
                current.append(QLatin1Char('%'));
                present = true;
            } else if (code == QLatin1Char('c')) {
                current.append(name);
                present = true;
            }
            // Every other code is removed, and contributes nothing to `present`.
        } else {
            current.append(c);
            present = true;
        }
    }
    if (inQuotes)
        return std::nullopt;
    finish();
    return arguments;
}

std::optional<DesktopEntry> parseDesktopEntry(const QString& text, const QString& locale)
{
    QHash<QString, QString> values;
    bool inGroup = false;
    bool sawGroup = false;

    const QStringList lines = text.split(QLatin1Char('\n'));
    for (const QString& rawLine : lines) {
        const QString line = rawLine.trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
            continue;
        if (line.startsWith(QLatin1Char('['))) {
            // Only the `[Desktop Entry]` group is read. Actions (`[Desktop Action ...]`) are separate groups
            // with their own `Name` and `Exec`, and reading them as the entry's would launch the wrong thing.
            inGroup = line == QLatin1String("[Desktop Entry]");
            sawGroup = sawGroup || inGroup;
            continue;
        }
        if (!inGroup)
            continue;
        const qsizetype eq = line.indexOf(QLatin1Char('='));
        if (eq <= 0)
            continue;
        const QString key = line.left(eq).trimmed();
        // The first occurrence wins: a key given twice is a malformed file, and which one a reader takes is
        // not specified, so the choice that cannot change with the order the reader happens to use is the
        // one written down here.
        if (!values.contains(key))
            values.insert(key, line.mid(eq + 1).trimmed());
    }

    if (!sawGroup)
        return std::nullopt;
    if (values.value(QStringLiteral("Type")) != QLatin1String("Application"))
        return std::nullopt;

    DesktopEntry entry;
    QString name = values.value(QStringLiteral("Name"));
    for (const QString& candidate : localeCandidates(locale)) {
        const QString localised = values.value(QStringLiteral("Name[%1]").arg(candidate));
        if (!localised.isEmpty()) {
            name = localised;
            break;
        }
    }
    entry.name = unescapeValue(name);
    if (entry.name.isEmpty())
        return std::nullopt;

    QString comment = values.value(QStringLiteral("Comment"));
    for (const QString& candidate : localeCandidates(locale)) {
        const QString localised = values.value(QStringLiteral("Comment[%1]").arg(candidate));
        if (!localised.isEmpty()) {
            comment = localised;
            break;
        }
    }
    entry.comment = unescapeValue(comment);

    const std::optional<QStringList> arguments = splitExec(unescapeValue(values.value(QStringLiteral("Exec"))),
                                                          entry.name);
    if (!arguments.has_value() || arguments->isEmpty() || arguments->first().isEmpty())
        return std::nullopt;
    entry.arguments = *arguments;

    entry.tryExec = unescapeValue(values.value(QStringLiteral("TryExec")));
    const auto isTrue = [&](const char* key) { return values.value(QLatin1String(key)) == QLatin1String("true"); };
    entry.hidden = isTrue("NoDisplay") || isTrue("Hidden");
    entry.terminal = isTrue("Terminal");
    entry.onlyShowIn = splitList(values.value(QStringLiteral("OnlyShowIn")));
    entry.notShowIn = splitList(values.value(QStringLiteral("NotShowIn")));
    return entry;
}

bool isOffered(const DesktopEntry& entry, const QStringList& currentDesktops)
{
    if (entry.hidden || entry.terminal)
        return false;
    const auto intersects = [&](const QStringList& list) {
        for (const QString& desktop : currentDesktops) {
            if (list.contains(desktop, Qt::CaseSensitive))
                return true;
        }
        return false;
    };
    if (!entry.onlyShowIn.isEmpty() && !intersects(entry.onlyShowIn))
        return false;
    if (intersects(entry.notShowIn))
        return false;
    return true;
}

int matchScore(const QString& query, const QString& text)
{
    if (query.isEmpty())
        return 0;
    const QString q = query.toCaseFolded();
    const QString t = text.toCaseFolded();

    if (t.startsWith(q))
        return 300;

    // A word of the text: the query follows a boundary — whitespace, `-`, `_`, `.` or `/`.
    for (qsizetype i = 1; i < t.size(); ++i) {
        const QChar before = t.at(i - 1);
        const bool boundary = before.isSpace() || before == QLatin1Char('-') || before == QLatin1Char('_')
            || before == QLatin1Char('.') || before == QLatin1Char('/');
        if (boundary && QStringView(t).mid(i).startsWith(q))
            return 200;
    }

    if (t.contains(q))
        return 100;

    // The characters in order, anywhere: the loosest match, ranked by how tightly the run sits.
    qsizetype from = 0;
    qsizetype first = -1;
    qsizetype last = -1;
    for (const QChar c : q) {
        const qsizetype found = t.indexOf(c, from);
        if (found < 0)
            return -1;
        if (first < 0)
            first = found;
        last = found;
        from = found + 1;
    }
    const qsizetype span = last - first + 1;
    const int bonus = static_cast<int>(std::max<qsizetype>(0, 50 - (span - q.size())));
    return 10 + std::min(bonus, 50);
}

}  // namespace quantum::apps
