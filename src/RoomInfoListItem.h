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

#pragma once

#include <QAction>
#include <QDateTime>
#include <QSharedPointer>
#include <QWidget>

#include <mtx/responses/sync.hpp>

#include "CacheStructs.h"
#include "UserSettingsPage.h"
#include "ui/Avatar.h"

class QMenu;
class RippleOverlay;

class RoomInfoListItem : public QWidget
{
        Q_OBJECT
        Q_PROPERTY(QColor highlightedBackgroundColor READ highlightedBackgroundColor WRITE
                     setHighlightedBackgroundColor)
        Q_PROPERTY(
          QColor hoverBackgroundColor READ hoverBackgroundColor WRITE setHoverBackgroundColor)
        Q_PROPERTY(QColor backgroundColor READ backgroundColor WRITE setBackgroundColor)

        Q_PROPERTY(QColor bubbleBgColor READ bubbleBgColor WRITE setBubbleBgColor)
        Q_PROPERTY(QColor bubbleFgColor READ bubbleFgColor WRITE setBubbleFgColor)

        Q_PROPERTY(QColor titleColor READ titleColor WRITE setTitleColor)
        Q_PROPERTY(QColor subtitleColor READ subtitleColor WRITE setSubtitleColor)

        Q_PROPERTY(QColor timestampColor READ timestampColor WRITE setTimestampColor)
        Q_PROPERTY(QColor highlightedTimestampColor READ highlightedTimestampColor WRITE
                     setHighlightedTimestampColor)
        Q_PROPERTY(QColor hoverTimestampColor READ hoverTimestampColor WRITE setHoverTimestampColor)

        Q_PROPERTY(
          QColor highlightedTitleColor READ highlightedTitleColor WRITE setHighlightedTitleColor)
        Q_PROPERTY(QColor highlightedSubtitleColor READ highlightedSubtitleColor WRITE
                     setHighlightedSubtitleColor)

        Q_PROPERTY(QColor hoverTitleColor READ hoverTitleColor WRITE setHoverTitleColor)
        Q_PROPERTY(QColor hoverSubtitleColor READ hoverSubtitleColor WRITE setHoverSubtitleColor)

        Q_PROPERTY(QColor mentionedColor READ mentionedColor WRITE setMentionedColor)
        Q_PROPERTY(QColor btnColor READ btnColor WRITE setBtnColor)
        Q_PROPERTY(QColor btnTextColor READ btnTextColor WRITE setBtnTextColor)

public:
        RoomInfoListItem(QString room_id, const RoomInfo &info, QWidget *parent = nullptr);

        void updateUnreadMessageCount(int count, int highlightedCount);
        void clearUnreadMessageCount() { updateUnreadMessageCount(0, 0); };

        short int calculateImportance() const;

        QString roomId() { return roomId_; }
        bool isPressed() const { return isPressed_; }
        int unreadMessageCount() const { return unreadMsgCount_; }

        void setAvatar(const QString &avatar_url);
        void setDescriptionMessage(const DescInfo &info);
        DescInfo lastMessageInfo() const { return lastMsgInfo_; }

        QColor highlightedBackgroundColor() const { return highlightedBackgroundColor_; }
        QColor hoverBackgroundColor() const { return hoverBackgroundColor_; }
        QColor hoverTitleColor() const { return hoverTitleColor_; }
        QColor hoverSubtitleColor() const { return hoverSubtitleColor_; }
        QColor hoverTimestampColor() const { return hoverTimestampColor_; }
        QColor backgroundColor() const { return backgroundColor_; }

        QColor highlightedTitleColor() const { return highlightedTitleColor_; }
        QColor highlightedSubtitleColor() const { return highlightedSubtitleColor_; }
        QColor highlightedTimestampColor() const { return highlightedTimestampColor_; }

        QColor titleColor() const { return titleColor_; }
        QColor subtitleColor() const { return subtitleColor_; }
        QColor timestampColor() const { return timestampColor_; }
        QColor btnColor() const { return btnColor_; }
        QColor btnTextColor() const { return btnTextColor_; }

        QColor bubbleFgColor() const { return bubbleFgColor_; }
        QColor bubbleBgColor() const { return bubbleBgColor_; }
        QColor mentionedColor() const { return mentionedFontColor_; }

        void setHighlightedBackgroundColor(QColor &color) { highlightedBackgroundColor_ = color; }
        void setHoverBackgroundColor(QColor &color) { hoverBackgroundColor_ = color; }
        void setHoverSubtitleColor(QColor &color) { hoverSubtitleColor_ = color; }
        void setHoverTitleColor(QColor &color) { hoverTitleColor_ = color; }
        void setHoverTimestampColor(QColor &color) { hoverTimestampColor_ = color; }
        void setBackgroundColor(QColor &color) { backgroundColor_ = color; }
        void setTimestampColor(QColor &color) { timestampColor_ = color; }

        void setHighlightedTitleColor(QColor &color) { highlightedTitleColor_ = color; }
        void setHighlightedSubtitleColor(QColor &color) { highlightedSubtitleColor_ = color; }
        void setHighlightedTimestampColor(QColor &color) { highlightedTimestampColor_ = color; }

        void setTitleColor(QColor &color) { titleColor_ = color; }
        void setSubtitleColor(QColor &color) { subtitleColor_ = color; }

        void setBtnColor(QColor &color) { btnColor_ = color; }
        void setBtnTextColor(QColor &color) { btnTextColor_ = color; }

        void setBubbleFgColor(QColor &color) { bubbleFgColor_ = color; }
        void setBubbleBgColor(QColor &color) { bubbleBgColor_ = color; }
        void setMentionedColor(QColor &color) { mentionedFontColor_ = color; }

        void setRoomName(const QString &name) { roomName_ = name; }
        void setRoomType(bool isInvite)
        {
                if (isInvite)
                        roomType_ = RoomType::Invited;
                else
                        roomType_ = RoomType::Joined;
        }

        bool isInvite() const { return roomType_ == RoomType::Invited; }
        void setReadState(bool hasUnreadMessages)
        {
                if (hasUnreadMessages_ != hasUnreadMessages) {
                        hasUnreadMessages_ = hasUnreadMessages;
                        update();
                }
        }

signals:
        void clicked(const QString &room_id);
        void leaveRoom(const QString &room_id);
        void acceptInvite(const QString &room_id);
        void declineInvite(const QString &room_id);

public slots:
        void setPressedState(bool state);

protected:
        void mousePressEvent(QMouseEvent *event) override;
        void paintEvent(QPaintEvent *event) override;
        void resizeEvent(QResizeEvent *event) override;
        void contextMenuEvent(QContextMenuEvent *event) override;

private:
        void init(QWidget *parent);
        QString roomName() { return roomName_; }

        RippleOverlay *ripple_overlay_;
        Avatar *avatar_;

        enum class RoomType
        {
                Joined,
                Invited,
        };

        RoomType roomType_ = RoomType::Joined;

        // State information for the invited rooms.
        mtx::responses::InvitedRoom invitedRoom_;

        QString roomId_;
        QString roomName_;

        DescInfo lastMsgInfo_;

        QMenu *menu_;
        QAction *leaveRoom_;

        bool isPressed_         = false;
        bool hasUnreadMessages_ = true;

        int unreadMsgCount_            = 0;
        int unreadHighlightedMsgCount_ = 0;

        QColor highlightedBackgroundColor_;
        QColor hoverBackgroundColor_;
        QColor backgroundColor_;

        QColor highlightedTitleColor_;
        QColor highlightedSubtitleColor_;

        QColor titleColor_;
        QColor subtitleColor_;

        QColor hoverTitleColor_;
        QColor hoverSubtitleColor_;

        QColor btnColor_;
        QColor btnTextColor_;

        QRectF acceptBtnRegion_;
        QRectF declineBtnRegion_;

        // Fonts
        QColor mentionedFontColor_;
        QFont unreadCountFont_;
        int bubbleDiameter_;

        QColor timestampColor_;
        QColor highlightedTimestampColor_;
        QColor hoverTimestampColor_;

        QColor bubbleBgColor_;
        QColor bubbleFgColor_;
};
