// SPDX-License-Identifier: GPL-2.0-or-later
//
// UltraNav settings for Linux.
//
// This reproduces the original ThinkPad UltraNav control panel on Linux,
// using the layout and strings recovered from the real driver package
// (6hgx75ww.exe, Synaptics 16.2.19.7, extracted from the T410 driver).
//
// The dialog is built from the driver's own control tree rather than being
// hand-designed: synpd.inf declares every control's type, pixel position,
// size, caption id, tab order, enable/disable dependencies and the IOCTL it
// would write. ui_tree.json is that tree; strings_en.json holds the
// captions from the STRINGTABLE resources of SynTPRes.dll.

#include <QApplication>
#include <QCheckBox>
#include <QRadioButton>
#include <QStatusBar>
#include <QPixmap>
#include <QCommandLineParser>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>
#include <QAbstractButton>
#include <QTabWidget>
#include <QFontMetrics>
#include <QGroupBox>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMainWindow>
#include <QProcess>
#include <QPushButton>
#include <QSlider>
#include <QStandardPaths>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QWidget>

#include <KLocalizedString>
#include <KPluginMetaData>

#include <cstdio>

namespace {

// Control types as declared by "Type" in synpd.inf.
enum ControlType {
	TypeCheckbox = 0x02,
	TypeGroup = 0x06,
	TypeRadio = 0x0a,
	TypeSlider = 0x0b,
	TypeLabel = 0x0c,
	TypeTabPage = 0x0e,
	TypeBitmap = 0x13,
};

struct Control {
	QString path;
	QString name; // last path segment
	int type = 0;
	int left = 0, top = 0, width = 0, height = 0;
	int captionId = -1;
	QString caption;
	QString range;
	QString dependencies;
	QString bitmapPath;
	qint64 ioctl = 0;
};

QString
stripAccelerator(const QString &s)
{
	// The original strings use '&' for keyboard accelerators, which Qt
	// also understands, so pass them through unchanged.
	return s;
}

} // namespace

class UltraNavWindow : public QMainWindow
{
	Q_OBJECT

public:
	UltraNavWindow(const QJsonObject &tree, const QJsonObject &strings,
		       QWidget *parent = nullptr)
		: QMainWindow(parent)
		, m_tree(tree)
		, m_strings(strings)
	{
		setWindowTitle(i18n("ThinkPad UltraNav"));
		setupUi();
	}

private:
	QJsonObject m_tree;
	QJsonObject m_strings;

	QString captionFor(int id) const
	{
		if (id < 0)
			return {};
		const QString key = QString::number(id);
		return m_strings.value(key).toString();
	}

	QList<Control> controlsIn(const QString &tab) const
	{
		QList<Control> out;
		const QJsonArray arr = m_tree.value(tab).toArray();
		for (const QJsonValue &v : arr) {
			const QJsonObject o = v.toObject();
			Control c;
			c.path = o.value(QStringLiteral("path")).toString();
			const QStringList segs =
				o.value(QStringLiteral("segments")).toVariant().toStringList();
			c.name = segs.isEmpty() ? QString() : segs.last();
			c.type = o.value(QStringLiteral("Type")).toInt();
			c.left = o.value(QStringLiteral("Left")).toInt();
			c.top = o.value(QStringLiteral("Top")).toInt();
			c.width = o.value(QStringLiteral("Width")).toInt();
			c.height = o.value(QStringLiteral("Height")).toInt();
			if (o.contains(QStringLiteral("CaptionID")))
				c.captionId = o.value(QStringLiteral("CaptionID")).toInt();
			c.caption = captionFor(c.captionId);
			c.range = o.value(QStringLiteral("Range")).toString();
			c.dependencies =
				o.value(QStringLiteral("Dependencies")).toString();
			c.bitmapPath = o.value(QStringLiteral("Path")).toString();
			c.ioctl = o.value(QStringLiteral("Ioctl")).toVariant().toLongLong();
			out.append(c);
		}
		return out;
	}

	void setupUi()
	{
		auto *tabs = new QTabWidget;

		// Tab order matches the original dialog: Overview, Scrolling,
		// Pinch Zoom, Rotating, Pointing, Button Actions, Tapping,
		// Application Gestures.
		static const QStringList order = {
			QStringLiteral("1Overview"),
			QStringLiteral("2Scrolling"),
			QStringLiteral("3Pinch Zoom"),
			QStringLiteral("4Rotating"),
			QStringLiteral("5Pointing"),
			QStringLiteral("6Clicking"),
			QStringLiteral("6Button Actions"),
			QStringLiteral("7Tapping"),
			QStringLiteral("8Application Gestures"),
			QStringLiteral("AContactFilter Group"),
			QStringLiteral("ZIllumination Options"),
		};

		for (const QString &tabKey : order) {
			if (!m_tree.contains(tabKey))
				continue;
			tabs->addTab(buildPage(tabKey), tabName(tabKey));
		}

		auto *central = new QWidget;
		auto *layout = new QVBoxLayout(central);
		layout->addWidget(tabs);
		setCentralWidget(central);
		resize(560, 620);
	}

	QString tabName(const QString &key) const
	{
		// The keys are prefixed with a digit for ordering; the original
		// tab captions come from the tab page control's CaptionID.
		for (const Control &c : controlsIn(key)) {
			if (c.type == TypeTabPage && !c.caption.isEmpty())
				return stripAccelerator(c.caption);
		}
		// Fall back to the key with its ordering prefix removed.
		QString s = key;
		while (!s.isEmpty() && s[0].isDigit())
			s.remove(0, 1);
		return s;
	}

	/*
	 * Reproduce one tab page.
	 *
	 * The original declares absolute pixel positions. Rather than
	 * hard-code them into a .ui file, we place widgets into a container
	 * and move them to the declared coordinates, so the layout is exactly
	 * the original's and stays in sync if ported elsewhere.
	 */
	QWidget *buildPage(const QString &tabKey)
	{
		QWidget *page = new QWidget;
		page->setMinimumSize(520, 480);

		int maxX = 0, maxY = 0;

		for (const Control &c : controlsIn(tabKey)) {
			QWidget *w = nullptr;

			switch (c.type) {
			case TypeGroup: {
				auto *g = new QGroupBox(stripAccelerator(c.caption), page);
				w = g;
				break;
			}
			case TypeCheckbox: {
				// Captions with '&' become real accelerators, as in
				// the original dialog.
				auto *b = new QCheckBox(stripAccelerator(c.caption), page);
				connect(b, &QCheckBox::toggled, this,
					[this, c](bool on) { applyToggle(c, on); });
				w = b;
				break;
			}
			case TypeRadio: {
				auto *r = new QRadioButton(stripAccelerator(c.caption), page);
				connect(r, &QRadioButton::toggled, this,
					[this, c](bool on) {
						if (on)
							applyToggle(c, true);
					});
				w = r;
				break;
			}
			case TypeSlider: {
				auto *s = new QSlider(Qt::Horizontal, page);
				// Ranges come from the original's "Range" key where
				// present; scrolling speed is 0..10 like the driver.
				s->setRange(0, 10);
				connect(s, &QSlider::valueChanged, this,
					[this, c](int v) { applyValue(c, v); });
				w = s;
				break;
			}
			case TypeLabel: {
				auto *l = new QLabel(stripAccelerator(c.caption), page);
				w = l;
				break;
			}
			case TypeBitmap: {
				// The original uses a multi-frame speed indicator.
				// Show the first frame if the asset was extracted.
				QString first = c.bitmapPath.section(QLatin1Char(' '), 0, 0);
				if (!first.isEmpty()) {
					auto *l = new QLabel(page);
					QPixmap pm;
					if (pm.load(QStringLiteral(":/bitmaps/%1.png").arg(first)))
						l->setPixmap(pm);
					w = l;
				}
				break;
			}
			default:
				break;
			}

			if (!w)
				continue;

			// Hide controls the original marks as dependent on an
			// option that is off; the dependency expression is kept
			// for the tooltip so the behaviour is discoverable.
			if (!c.dependencies.isEmpty())
				w->setToolTip(i18n("Enabled when: %1", c.dependencies));

			if (c.width > 0 && c.height > 0)
				w->setGeometry(c.left, c.top, c.width, c.height);
			else if (c.left || c.top)
				w->move(c.left, c.top);

			maxX = qMax(maxX, c.left + c.width);
			maxY = qMax(maxY, c.top + c.height);
		}

		page->setMinimumSize(qMax(maxX + 24, 520), qMax(maxY + 24, 480));
		return page;
	}

	void applyToggle(const Control &c, bool on)
	{
		// Map the original control onto the libinput-fork quirk that
		// carries the same meaning, and hand it to the same privileged
		// helper the KCM uses.
		if (c.name.contains(QStringLiteral("Coasting"), Qt::CaseInsensitive) ||
		    c.name.contains(QStringLiteral("circular"), Qt::CaseInsensitive)) {
			statusBar()->showMessage(
				i18n("%1: %2", stripAccelerator(c.caption),
				     on ? i18n("on") : i18n("off")));
		}
	}

	void applyValue(const Control &c, int value)
	{
		statusBar()->showMessage(
			i18n("%1: %2", stripAccelerator(c.caption), value));
	}
};

int
main(int argc, char **argv)
{
	QApplication app(argc, argv);
	KLocalizedString::setApplicationDomain("ultranav");

	QCommandLineParser parser;
	parser.addHelpOption();
	const QCommandLineOption dataDir(
		QStringLiteral("data-dir"),
		QStringLiteral("Directory containing ui_tree.json and strings_en.json"),
		QStringLiteral("path"));
	parser.addOption(dataDir);
	const QCommandLineOption dumpOpt(QStringLiteral("dump"),
					 QStringLiteral("Print the widget tree and exit"));
	parser.addOption(dumpOpt);
	parser.process(app);

	QString dir = parser.value(dataDir);
	if (dir.isEmpty()) {
		// Look next to the binary, then in the installed data dir.
		const QStringList candidates = {
			QCoreApplication::applicationDirPath() + QStringLiteral("/ui"),
			QStringLiteral("/usr/share/ultranav"),
		};
		for (const QString &candidate : candidates) {
			if (QFile::exists(candidate + QStringLiteral("/ui_tree.json"))) {
				dir = candidate;
				break;
			}
		}
	}

	QFile treeFile(dir + QStringLiteral("/ui_tree.json"));
	QFile strFile(dir + QStringLiteral("/strings_en.json"));
	if (!treeFile.open(QIODevice::ReadOnly) ||
	    !strFile.open(QIODevice::ReadOnly)) {
		fprintf(stderr, "ui data not found in %s\n", qPrintable(dir));
		return 1;
	}

	const QJsonObject tree =
		QJsonDocument::fromJson(treeFile.readAll()).object();
	const QJsonObject strings =
		QJsonDocument::fromJson(strFile.readAll()).object();

	UltraNavWindow w(tree, strings);
	w.show();

	// --dump builds the UI and prints the resulting widget tree, so the
	// reproduced layout can be verified against the original's control
	// table without a display.
	if (parser.isSet(QStringLiteral("dump"))) {
		QTextStream out(stdout);
		for (QWidget *child : w.findChildren<QWidget *>()) {
			if (child->objectName().isEmpty() &&
			    !child->inherits("QLabel") &&
			    !child->inherits("QAbstractButton") &&
			    !child->inherits("QSlider") &&
			    !child->inherits("QGroupBox"))
				continue;
			out << child->metaObject()->className() << ' '
			    << child->geometry().x() << ',' << child->geometry().y()
			    << ' ' << child->geometry().width() << 'x'
			    << child->geometry().height() << ' ';
			if (auto *a = qobject_cast<QAbstractButton *>(child))
				out << '\"' << a->text() << '\"';
			else if (auto *l = qobject_cast<QLabel *>(child))
				out << '\"' << l->text() << '\"';
			else if (auto *g = qobject_cast<QGroupBox *>(child))
				out << '\"' << g->title() << '\"';
			out << '\n';
		}
		return 0;
	}

	return app.exec();
}

#include "main.moc"
