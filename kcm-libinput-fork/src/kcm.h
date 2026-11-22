// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <KCModule>

#include <memory>

class LibinputForkKcm : public KCModule
{
	Q_OBJECT

public:
	explicit LibinputForkKcm(QObject *parent, const KPluginMetaData &data);
	~LibinputForkKcm() override;

	void load() override;
	void save() override;
	void defaults() override;

private:
	class Private;
	friend class Private;
	std::unique_ptr<Private> d;
};
