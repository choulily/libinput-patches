// SPDX-License-Identifier: GPL-2.0-or-later
//
// Touchpad settings for the libinput fork.
//
// This module does not talk to KWin for the fork's features, because KWin
// has no properties for them. Instead it writes the two configuration
// files that both KWin and libinput already read:
//
//   /etc/libinput/local-overrides.quirks  (via pkexec)
//       circular scroll trigger + distance, momentum parameters
//   ~/.config/kcminputrc
//       ScrollMethod, which selects circular scrolling as a method
//
// Both are configuration, so they survive package upgrades. The settings
// take effect the next time KWin opens the device, i.e. on login or on
// plugging the touchpad in.
//
// Device enumeration still uses KWin's D-Bus interface, so the module
// always lists the same touchpads as the rest of the system, and so it can
// show whether the running libinput actually has the fork (properties are
// absent on stock libinput).

#include "kcm.h"

#include <QComboBox>
#include <algorithm>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDir>
#include <QFile>
#include <QGroupBox>
#include <QLabel>
#include <QProcess>
#include <QPushButton>
#include <QSlider>
#include <QStandardPaths>
#include <QTextStream>
#include <QVBoxLayout>

#include <KLocalizedString>
#include <KPluginFactory>

#include "ui_main.h"

K_PLUGIN_CLASS_WITH_JSON(LibinputForkKcm, "kcm_libinputfork.json")

namespace {
// libinput's enum libinput_config_scroll_method.
constexpr uint32_t ScrollMethodTwoFinger = 1;
constexpr uint32_t ScrollMethodEdge = 2;
constexpr uint32_t ScrollMethodCircular = 8;

constexpr const char *QuirksPath = "/etc/libinput/local-overrides.quirks";
constexpr const char *QuirksHelper = "libinput-fork-apply";

// Must match the enum in the fork's libinput.h.
constexpr int TriggerAnyEdge = 0;

QString
kwinObjectPath(const QString &sysName)
{
	return QStringLiteral("/org/kde/KWin/InputDevice/") + sysName;
}

/*
 * Is the installed libinput the fork?
 *
 * The fork's public API is the only reliable signal: circular scrolling is
 * selected through the ordinary ScrollMethod key, so neither KWin nor the
 * scroll-method bitmask distinguishes the two libraries.
 *
 * Reading the library is cheap and needs no privileges. The result is
 * cached because the library cannot change while the session runs.
 */
bool
libinputForkPresent()
{
	static const bool present = [] {
		const QStringList candidates = {
			QStringLiteral("/usr/lib/libinput.so.10"),
			QStringLiteral("/usr/lib/x86_64-linux-gnu/libinput.so.10"),
			QStringLiteral("/usr/lib64/libinput.so.10"),
		};

		for (const QString &path : candidates) {
			QFile lib(path);
			// Symbols live in the read-only segment near the start;
			// reading whole multi-megabyte libraries is wasteful.
			if (!lib.open(QIODevice::ReadOnly))
				continue;
			const QByteArray head = lib.read(4 * 1024 * 1024);
			if (head.contains("libinput_device_config_momentum_is_available"))
				return true;
		}
		return false;
	}();
	return present;
}
} // namespace

class LibinputForkKcm::Private
{
public:
	Ui::LibinputForkConfig ui;
	QWidget *widget = nullptr;

	bool loading = false;
	bool forkPresent = false;
	QString currentSysName;
	QString currentDeviceName;

	struct DeviceInfo {
		QString sysName;
		QString name;
		bool isTouchpad = false;
		bool supportsCircular = false;
		bool supportsMomentum = false;
	};

	QList<DeviceInfo> devices;

	/* Advanced quirk attributes are not in the GUI. They are read from
	 * the existing section and written back unchanged, so editing the
	 * page does not discard values set by hand. */
	QHash<QString, QString> advancedValues;

	void populateDevices();
	bool queryDevice(const QString &sysName, DeviceInfo *info);
	void loadDevice(const DeviceInfo &info);
	void updateWidgets();
	QString buildQuirksSection() const;
	void showStatus(const QString &message, bool error);

	bool writeQuirks(const QString &content);
	bool setScrollMethod(const QString &deviceName, uint32_t method);
};

LibinputForkKcm::LibinputForkKcm(QObject *parent, const KPluginMetaData &data)
	: KCModule(parent, data)
	, d(new Private)
{
}

LibinputForkKcm::~LibinputForkKcm() = default;

// ---------------------------------------------------------------------------
// Device enumeration through KWin
// ---------------------------------------------------------------------------

void
LibinputForkKcm::Private::populateDevices()
{
	devices.clear();
	forkPresent = false;

	QDBusInterface manager(QStringLiteral("org.kde.KWin"),
			       QStringLiteral("/org/kde/KWin/InputDevice"),
			       QStringLiteral("org.kde.KWin.InputDeviceManager"),
			       QDBusConnection::sessionBus());
	if (!manager.isValid())
		return;

	QDBusReply<QStringList> reply = manager.call(QStringLiteral("ListPointers"));
	if (!reply.isValid())
		return;

	for (const QString &sysName : reply.value()) {
		DeviceInfo info;
		if (queryDevice(sysName, &info))
			devices.append(info);
	}

	// Touchpads first, then anything else that supports the features. The
	// list otherwise starts with whichever mouse KWin happened to report
	// first, which is wrong for a touchpad settings page.
	std::stable_sort(devices.begin(), devices.end(),
			 [](const DeviceInfo &a, const DeviceInfo &b) {
				 return a.isTouchpad && !b.isTouchpad;
			 });
}

bool
LibinputForkKcm::Private::queryDevice(const QString &sysName, DeviceInfo *info)
{
	QDBusInterface iface(QStringLiteral("org.kde.KWin"),
			     kwinObjectPath(sysName),
			     QStringLiteral("org.freedesktop.DBus.Properties"),
			     QDBusConnection::sessionBus());
	if (!iface.isValid())
		return false;

	auto get = [&iface](const QString &name) -> QVariant {
		QDBusReply<QDBusVariant> r = iface.call(
			QStringLiteral("Get"),
			QStringLiteral("org.kde.KWin.InputDevice"),
			name);
		return r.isValid() ? r.value().variant() : QVariant();
	};

	if (!get(QStringLiteral("pointer")).toBool())
		return false;

	// KWin has no "is touchpad" property. Scroll-method support is the
	// reliable signal: only touchpads advertise it, mice and trackpoints
	// do not. Devices without any scroll method are not what this page is
	// for, so skip them entirely.
	info->isTouchpad =
		get(QStringLiteral("supportsScrollTwoFinger")).toBool() ||
		get(QStringLiteral("supportsScrollEdge")).toBool();
	if (!info->isTouchpad)
		return false;

	info->sysName = sysName;
	info->name = get(QStringLiteral("name")).toString();
	if (info->name.isEmpty())
		info->name = sysName;

	// Whether circular scrolling and momentum are usable depends on the
	// installed libinput, not on KWin. Unpatched KWin reports the method
	// bitmask without knowing the circular bit, so asking it would always
	// say "unsupported". Instead probe the library: only the fork exports
	// these symbols.
	if (!libinputForkPresent())
		return false;

	info->supportsCircular = true;
	info->supportsMomentum = true;
	forkPresent = true;

	return true;
}

// ---------------------------------------------------------------------------
// Loading the current configuration
// ---------------------------------------------------------------------------

void
LibinputForkKcm::Private::loadDevice(const DeviceInfo &info)
{
	loading = true;

	// Defaults match the fork's own.
	int trigger = TriggerAnyEdge;
	double distance = 5.7;
	bool momentum = false;
	int strength = 100;
	int velocity = 120;

	// Read back whatever section this module previously wrote for the
	// device, so the UI reflects reality without needing KWin.
	QFile f(QString::fromLatin1(QuirksPath));
	if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
		QTextStream in(&f);
		bool inSection = false;
		while (!in.atEnd()) {
			const QString line = in.readLine().trimmed();
			if (line.startsWith(QLatin1Char('[')) && line.endsWith(QLatin1Char(']'))) {
				inSection = line.mid(1, line.size() - 2).endsWith(info.name);
				continue;
			}
			if (!inSection)
				continue;

			const int eq = line.indexOf(QLatin1Char('='));
			if (eq < 0)
				continue;
			const QString key = line.left(eq);
			const QString value = line.mid(eq + 1);

			if (key == QLatin1String("AttrCircularScrollTrigger"))
				trigger = value.toInt();
			else if (key == QLatin1String("AttrCircularScrollDistance"))
				distance = value.toDouble();
			else if (key == QLatin1String("AttrPointerMomentumEnabled"))
				momentum = value == QLatin1String("1");
			else if (key == QLatin1String("AttrPointerMomentumStrength"))
				strength = value.toInt();
			else if (key == QLatin1String("AttrPointerMomentumMinVelocity"))
				velocity = value.toInt();
			else if (key.startsWith(QLatin1String("AttrPointerMomentumDecayCurve")) ||
				 key.startsWith(QLatin1String("AttrScrollSpeed")))
				advancedValues.insert(key, value);
		}
	}

	ui.circularGroup->setChecked(info.supportsCircular);
	ui.momentumGroup->setChecked(momentum);

	const int idx = ui.triggerCombo->findData(trigger);
	ui.triggerCombo->setCurrentIndex(idx >= 0 ? idx : 0);

	ui.distanceSlider->setValue(qBound(1, qRound(distance), 90));
	ui.distanceValue->setText(i18nc("@item:valuesuffix degrees", "%1°",
					QString::number(distance, 'f', 1)));

	ui.strengthSlider->setValue(qBound(10, strength, 400));
	ui.strengthValue->setText(i18n("%1%", ui.strengthSlider->value()));

	ui.velocitySlider->setValue(qBound(0, velocity, 600));
	ui.velocityValue->setText(i18nc("@item:valuesuffix speed units", "%1",
					ui.velocitySlider->value()));

	updateWidgets();
	loading = false;
}

void
LibinputForkKcm::Private::updateWidgets()
{
	const bool circular = ui.circularGroup->isChecked();
	ui.triggerCombo->setEnabled(circular);
	ui.distanceSlider->setEnabled(circular);
	ui.triggerLabel->setEnabled(circular);
	ui.distanceLabel->setEnabled(circular);

	const bool momentum = ui.momentumGroup->isChecked();
	ui.strengthSlider->setEnabled(momentum);
	ui.strengthLabel->setEnabled(momentum);
	ui.strengthValue->setEnabled(momentum);
	ui.velocitySlider->setEnabled(momentum);
	ui.minVelocityLabel->setEnabled(momentum);
	ui.velocityValue->setEnabled(momentum);
}

void
LibinputForkKcm::Private::showStatus(const QString &message, bool error)
{
	ui.statusLabel->setText(message);
	ui.statusLabel->setStyleSheet(error ? QStringLiteral("color: #c0392b;")
					    : QString());
}

// ---------------------------------------------------------------------------
// Writing configuration
// ---------------------------------------------------------------------------

QString
LibinputForkKcm::Private::buildQuirksSection() const
{
	QString s;
	QTextStream out(&s);
	out << "[libinput-fork: " << currentDeviceName << "]\n";
	out << "MatchName=" << currentDeviceName << "\n";

	if (ui.circularGroup->isChecked()) {
		out << "AttrCircularScrollEnabled=1\n";
		out << "AttrCircularScrollTrigger="
		    << ui.triggerCombo->currentData().toInt() << "\n";
		out << "AttrCircularScrollDistance="
		    << QString::number(ui.distanceSlider->value(), 'f', 1) << "\n";
	}

	if (ui.momentumGroup->isChecked()) {
		out << "AttrPointerMomentumEnabled=1\n";
		out << "AttrPointerMomentumStrength="
		    << ui.strengthSlider->value() << "\n";
		out << "AttrPointerMomentumMinVelocity="
		    << ui.velocitySlider->value() << "\n";
	}

	// Advanced values are not in the GUI. Preserve whatever the user set
	// by hand instead of dropping it when the page saves.
	for (const QString &key : {QStringLiteral("AttrPointerMomentumDecayCurve"),
				   QStringLiteral("AttrScrollSpeed2FG"),
				   QStringLiteral("AttrScrollSpeedEdge"),
				   QStringLiteral("AttrScrollSpeedCircular")}) {
		if (advancedValues.contains(key))
			out << key << '=' << advancedValues.value(key) << '\n';
	}

	return s;
}

bool
LibinputForkKcm::Private::writeQuirks(const QString &content)
{
	// Write through a temp file and pkexec so the user gets exactly one
	// password prompt, and the target file is replaced atomically.
	const QString tmpDir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
	const QString tmpPath = tmpDir + QStringLiteral("/libinput-fork-quirks-")
		+ QString::number(QCoreApplication::applicationPid());

	QFile tmp(tmpPath);
	if (!tmp.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
		showStatus(i18n("Could not write a temporary file in %1.", tmpDir), true);
		return false;
	}
	QTextStream(&tmp) << content;
	tmp.close();

	const QString helper = QStandardPaths::findExecutable(QString::fromLatin1(QuirksHelper));
	if (helper.isEmpty()) {
		showStatus(i18n("%1 was not found, so the settings could not be applied.",
				QString::fromLatin1(QuirksHelper)),
			   true);
		QFile::remove(tmpPath);
		return false;
	}

	QProcess proc;
	proc.start(QStringLiteral("pkexec"),
		   {helper, QStringLiteral("--write-quirks"), tmpPath});
	proc.waitForFinished(120000);
	QFile::remove(tmpPath);

	const QString err = QString::fromUtf8(proc.readAllStandardError()).trimmed();
	if (proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0) {
		showStatus(err.isEmpty()
				   ? i18n("Applying the settings was cancelled.")
				   : i18n("Could not apply the settings: %1", err),
			   true);
		return false;
	}

	return true;
}

bool
LibinputForkKcm::Private::setScrollMethod(const QString &deviceName, uint32_t method)
{
	const QString helper = QStandardPaths::findExecutable(QString::fromLatin1(QuirksHelper));
	if (helper.isEmpty())
		return false;

	// kcminputrc is per-user, so no privilege needed, but the helper does
	// the merging (and preserves unrelated keys) for us.
	QProcess proc;
	proc.start(helper,
		   {QStringLiteral("--set-scroll-method"), QString::number(method),
		    QStringLiteral("--device-match"), deviceName});
	proc.waitForFinished(30000);
	return proc.exitStatus() == QProcess::NormalExit && proc.exitCode() == 0;
}

// ---------------------------------------------------------------------------
// KCModule API
// ---------------------------------------------------------------------------

void
LibinputForkKcm::load()
{
	if (!d->widget) {
		d->widget = new QWidget(widget());
		auto *layout = new QVBoxLayout(widget());
		layout->setContentsMargins(0, 0, 0, 0);
		d->ui.setupUi(d->widget);
		layout->addWidget(d->widget);
	}

	d->populateDevices();

	d->ui.deviceCombo->clear();
	for (const auto &info : std::as_const(d->devices))
		d->ui.deviceCombo->addItem(info.name, info.sysName);

	d->ui.triggerCombo->clear();
	d->ui.triggerCombo->addItem(i18n("Any edge"), 0);
	d->ui.triggerCombo->addItem(i18n("Top edge"), 1);
	d->ui.triggerCombo->addItem(i18n("Top right corner"), 2);
	d->ui.triggerCombo->addItem(i18n("Right edge"), 3);
	d->ui.triggerCombo->addItem(i18n("Bottom right corner"), 4);
	d->ui.triggerCombo->addItem(i18n("Bottom edge"), 5);
	d->ui.triggerCombo->addItem(i18n("Bottom left corner"), 6);
	d->ui.triggerCombo->addItem(i18n("Left edge"), 7);
	d->ui.triggerCombo->addItem(i18n("Top left corner"), 8);

	connect(d->ui.deviceCombo, qOverload<int>(&QComboBox::currentIndexChanged),
		this, [this](int index) {
			if (index < 0 || index >= d->devices.size())
				return;
			d->currentSysName = d->devices.at(index).sysName;
			d->currentDeviceName = d->devices.at(index).name;
			d->loadDevice(d->devices.at(index));
			d->showStatus(QString(), false);
		});

	connect(d->ui.circularGroup, &QGroupBox::toggled, this, [this](bool) {
		if (!d->loading)
			d->updateWidgets();
		markAsChanged();
	});
	connect(d->ui.momentumGroup, &QGroupBox::toggled, this, [this](bool) {
		if (!d->loading)
			d->updateWidgets();
		markAsChanged();
	});
	connect(d->ui.triggerCombo, qOverload<int>(&QComboBox::currentIndexChanged),
		this, [this](int) { markAsChanged(); });
	connect(d->ui.distanceSlider, &QSlider::valueChanged, this, [this](int v) {
		d->ui.distanceValue->setText(
			i18nc("@item:valuesuffix degrees", "%1°",
			      QString::number(v, 'f', 1)));
		markAsChanged();
	});
	connect(d->ui.strengthSlider, &QSlider::valueChanged, this, [this](int v) {
		d->ui.strengthValue->setText(i18n("%1%", v));
		markAsChanged();
	});
	connect(d->ui.velocitySlider, &QSlider::valueChanged, this, [this](int v) {
		d->ui.velocityValue->setText(
			i18nc("@item:valuesuffix speed units", "%1", v));
		markAsChanged();
	});

	if (d->devices.isEmpty()) {
		d->ui.deviceCombo->setEnabled(false);
		d->ui.circularGroup->setEnabled(false);
		d->ui.momentumGroup->setEnabled(false);
		d->showStatus(i18n("No touchpad was found."), true);
		return;
	}

	d->ui.deviceCombo->setEnabled(true);
	d->ui.circularGroup->setEnabled(true);
	d->ui.momentumGroup->setEnabled(true);
	d->ui.deviceCombo->setCurrentIndex(0);
	d->currentSysName = d->devices.first().sysName;
	d->currentDeviceName = d->devices.first().name;
	d->loadDevice(d->devices.first());

	if (!d->forkPresent) {
		d->showStatus(
			i18n("The libinput fork is not loaded, so these settings "
			     "cannot take effect. They will be written to %1 and "
			     "used as soon as the fork is installed.",
			     QString::fromLatin1(QuirksPath)),
			true);
	} else {
		d->showStatus(QString(), false);
	}
}

void
LibinputForkKcm::save()
{
	if (d->currentSysName.isEmpty())
		return;

	if (!d->writeQuirks(d->buildQuirksSection()))
		return;

	// Circular scrolling is selected as a scroll method; the quirks file
	// only supplies its parameters.
	if (d->ui.circularGroup->isChecked())
		d->setScrollMethod(d->currentDeviceName, ScrollMethodCircular);
	else
		d->setScrollMethod(d->currentDeviceName, ScrollMethodTwoFinger);

	d->showStatus(
		i18n("Saved. The change applies the next time the touchpad is "
		     "opened — log out and back in, or unplug and replug an "
		     "external touchpad."),
		false);
}

void
LibinputForkKcm::defaults()
{
	d->loading = true;
	d->ui.circularGroup->setChecked(false);
	d->ui.momentumGroup->setChecked(false);
	d->ui.triggerCombo->setCurrentIndex(0);
	d->ui.distanceSlider->setValue(6);
	d->ui.strengthSlider->setValue(100);
	d->ui.velocitySlider->setValue(120);
	d->updateWidgets();
	d->loading = false;
	markAsChanged();
}

#include "kcm.moc"
