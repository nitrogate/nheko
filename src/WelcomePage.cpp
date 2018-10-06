/*
 * nheko Copyright (C) 2017  Konstantinos Sideris <siderisk@auth.gr>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <QLabel>
#include <QLayout>
#include <QStyleOption>

#include "Config.h"
#include "WelcomePage.h"
#include "ui/RaisedButton.h"
#include "ui/TextLabel.h"

WelcomePage::WelcomePage(QWidget *parent)
  : QWidget(parent)
{
        auto topLayout_ = new QVBoxLayout(this);
        topLayout_->setSpacing(20);
        topLayout_->setAlignment(Qt::AlignCenter);

        QFont headingFont;
        headingFont.setPointSizeF(headingFont.pointSizeF() * 2);
        QFont subTitleFont;
        subTitleFont.setPointSizeF(subTitleFont.pointSizeF() * 1.5);

        QIcon icon;
        icon.addFile(":/logos/splash.png");

        auto logo_ = new QLabel(this);
        logo_->setPixmap(icon.pixmap(256));
        logo_->setAlignment(Qt::AlignCenter);

        QString heading(tr("Welcome to nheko! The desktop client for the Matrix protocol."));
        QString main(tr("Enjoy your stay!"));

        auto intoTxt_ = new TextLabel(heading, this);
        intoTxt_->setFont(headingFont);
        intoTxt_->setAlignment(Qt::AlignCenter);

        auto subTitle = new TextLabel(main, this);
        subTitle->setFont(subTitleFont);
        subTitle->setAlignment(Qt::AlignCenter);

        topLayout_->addStretch(1);
        topLayout_->addWidget(logo_);
        topLayout_->addWidget(intoTxt_);
        topLayout_->addWidget(subTitle);

        auto btnLayout_ = new QHBoxLayout();
        btnLayout_->setSpacing(20);
        btnLayout_->setContentsMargins(0, 20, 0, 20);

        const int fontHeight   = QFontMetrics{subTitleFont}.height();
        const int buttonHeight = fontHeight * 2.5;
        const int buttonWidth  = fontHeight * 8;

        auto registerBtn = new RaisedButton(tr("REGISTER"), this);
        registerBtn->setMinimumSize(buttonWidth, buttonHeight);
        registerBtn->setFontSize(subTitleFont.pointSizeF());
        registerBtn->setCornerRadius(conf::btn::cornerRadius);

        auto loginBtn = new RaisedButton(tr("LOGIN"), this);
        loginBtn->setMinimumSize(buttonWidth, buttonHeight);
        loginBtn->setFontSize(subTitleFont.pointSizeF());
        loginBtn->setCornerRadius(conf::btn::cornerRadius);

        btnLayout_->addStretch(1);
        btnLayout_->addWidget(registerBtn);
        btnLayout_->addWidget(loginBtn);
        btnLayout_->addStretch(1);

        topLayout_->addLayout(btnLayout_);
        topLayout_->addStretch(1);

        connect(registerBtn, &QPushButton::clicked, this, &WelcomePage::userRegister);
        connect(loginBtn, &QPushButton::clicked, this, &WelcomePage::userLogin);
}

void
WelcomePage::paintEvent(QPaintEvent *)
{
        QStyleOption opt;
        opt.init(this);
        QPainter p(this);
        style()->drawPrimitive(QStyle::PE_Widget, &opt, &p, this);
}
