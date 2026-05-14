void handleWake(const std::string& device_id, const std::string& cmd) {
    try {
        if (!ctx_.wake_manager) {
            response(500, {{"error", "wake_manager_not_ready"}});
            return;
        }

        std::string msg_id;

        if (cmd == "lowpower_wakeup") {
            msg_id = ctx_.wake_manager->sendLowPowerWake(device_id);
        } else {
            msg_id = ctx_.wake_manager->sendWake(
                device_id,
                cmd,
                {
                    {"resolution", "640x480"},
                    {"quality", 80}
                }
            );
        }

        response(200, {
            {"result", "sent"},
            {"device_id", device_id},
            {"cmd", cmd},
            {"msg_id", msg_id}
        });
    } catch (const std::exception& e) {
        response(409, {
            {"error", e.what()},
            {"device_id", device_id}
        });
    }
}