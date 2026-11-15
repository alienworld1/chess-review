#pragma once
#include "app/ReviewWorkspaceController.h"
namespace chess_review::app {
class ReviewWorkspaceTestAccess {
  public:
    static QTimer& liveTimer(ReviewWorkspaceController& workspace) { return workspace.liveTimer_; }
    static QUuid liveRequest(const ReviewWorkspaceController& workspace) {
        return workspace.liveRead_ ? workspace.liveRead_->id : QUuid{};
    }
    static QUuid explicitRequest(const ReviewWorkspaceController& workspace) {
        return workspace.pending_;
    }
};
} // namespace chess_review::app
