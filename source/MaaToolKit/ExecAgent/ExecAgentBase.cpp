#include "ExecAgentBase.h"

#include "MaaFramework/MaaAPI.h"
#include "Utils/IOStream/ChildPipeIOStream.h"
#include "Utils/ImageIo.h"
#include "Utils/Logger.h"

MAA_TOOLKIT_NS_BEGIN

ExecAgentBase::~ExecAgentBase()
{
    for (auto& child : detached_child_) {
        child.release();
    }
}

bool ExecAgentBase::register_executor(MaaInstanceHandle handle, std::string_view name, std::filesystem::path exec_path,
                                      std::vector<std::string> exec_args, TextTransferMode text_mode,
                                      ImageTransferMode image_mode)
{
    LogFunc << VAR_VOIDP(handle) << VAR(name) << VAR(exec_path) << VAR(exec_args) << VAR(text_mode) << VAR(image_mode);

    auto [iter, inserted] = exec_data_.insert_or_assign( //
        std::string(name), ExecData {
                               .name = std::string(name),
                               .exec_path = std::move(exec_path),
                               .exec_args = std::move(exec_args),
                               .text_mode = text_mode,
                               .image_mode = image_mode,
                           });
    if (!inserted || iter == exec_data_.end()) {
        LogError << "insert failed" << VAR(name);
    }

    bool registered = register_for_maa_inst(handle, name, iter->second);
    if (!registered) {
        LogError << "register failed" << VAR(name);
        exec_data_.erase(iter);
        return false;
    }

    return true;
}

bool ExecAgentBase::unregister_executor(MaaInstanceHandle handle, std::string_view name)
{
    LogFunc << VAR_VOIDP(handle) << VAR(name);

    bool ret = unregister_for_maa_inst(handle, name);
    ret &= exec_data_.erase(std::string(name)) > 0;

    return ret;
}

std::optional<json::value> ExecAgentBase::run_executor(const std::filesystem::path& exec_path,
                                                       const std::vector<std::string>& exec_args,
                                                       TextTransferMode text_mode, ImageTransferMode image_mode)
{
    auto searched_path = boost::process::search_path(exec_path);
    if (!std::filesystem::exists(searched_path)) {
        LogError << "path not exists" << VAR(searched_path);
        return std::nullopt;
    }

    ChildPipeIOStream child(searched_path, exec_args);

    std::optional<json::value> result_opt = std::nullopt;
    switch (text_mode) {
    case TextTransferMode::StdIO:
        result_opt = handle_ipc(child, image_mode);
        break;

    case TextTransferMode::FileIO:
        LogError << "not implemented";
        return std::nullopt;

    default:
        LogError << "not implemented";
        return std::nullopt;
    }

    auto& result = *result_opt;
    bool detach = result.get("detach", false);
    if (detach) {
        detached_child_.emplace_back(std::move(child));
    }
    else if (!child.release()) { // join
        LogError << "join and release failed";
        return std::nullopt;
    }

    bool ret = result.get("return", false);
    if (!ret) {
        LogInfo << "return false" << VAR(result);
        return std::nullopt;
    }

    return result;
}

std::optional<json::value> ExecAgentBase::handle_ipc(IOStream& ios, ImageTransferMode image_mode)
{
    std::ignore = image_mode;

    json::value result;
    while (ios.is_open()) {
        std::string line = ios.read_until("\n");
        LogInfo << "read line:" << line;

        auto json_opt = json::parse(line);
        if (!json_opt) {
            LogWarn << "parse json failed" << VAR(line);
            continue;
        }

        bool break_loop = json_opt->find<bool>("return").value_or(false);
        if (break_loop) {
            result = *json_opt;
            LogInfo << "break loop" << VAR(result);
            break;
        }

        std::string write = handle_command(*json_opt);
        ios.write(write + "\n");
    }

    LogDebug << VAR(result);
    return result;
}

std::string ExecAgentBase::handle_command(const json::value& cmd)
{
    static const std::map<std::string, std::function<json::value(const json::value&)>> cmd_map = {
        { "RunTask", std::bind(&ExecAgentBase::ctx_run_task, this, std::placeholders::_1) },
        { "RunRecognizer", std::bind(&ExecAgentBase::ctx_run_recognizer, this, std::placeholders::_1) },
        { "RunAction", std::bind(&ExecAgentBase::ctx_run_action, this, std::placeholders::_1) },
        { "Click", std::bind(&ExecAgentBase::ctx_click, this, std::placeholders::_1) },
        { "Swipe", std::bind(&ExecAgentBase::ctx_swipe, this, std::placeholders::_1) },
        { "PressKey", std::bind(&ExecAgentBase::ctx_press_key, this, std::placeholders::_1) },
        { "TouchDown", std::bind(&ExecAgentBase::ctx_touch_down, this, std::placeholders::_1) },
        { "TouchMove", std::bind(&ExecAgentBase::ctx_touch_move, this, std::placeholders::_1) },
        { "TouchUp", std::bind(&ExecAgentBase::ctx_touch_up, this, std::placeholders::_1) },
        { "Screencap", std::bind(&ExecAgentBase::ctx_screencap, this, std::placeholders::_1) },
        { "GetTaskResult", std::bind(&ExecAgentBase::ctx_get_task_result, this, std::placeholders::_1) },
    };

    auto func_opt = cmd.find<std::string>("function");
    if (!func_opt) {
        LogError << "no function" << VAR(cmd);
        return invalid_json().to_string();
    }

    auto iter = cmd_map.find(*func_opt);
    if (iter == cmd_map.end()) {
        LogError << "no function" << VAR(*func_opt) << VAR(cmd);
        return invalid_json().to_string();
    }

    return iter->second(cmd).to_string();
}

json::value ExecAgentBase::ctx_run_task(const json::value& cmd)
{
    auto ctx = get_sync_context(cmd);
    if (!ctx) {
        LogError << "sync context not found" << VAR(cmd);
        return invalid_json();
    }

    std::string task_name = cmd.get("task_name", std::string());
    if (task_name.empty()) {
        LogError << "task name empty" << VAR(cmd);
        return invalid_json();
    }

    std::string task_param = cmd.get("task_param", std::string());
    if (task_param.empty()) {
        task_param = json::object().to_string();
    }

    bool ret = MaaSyncContextRunTask(ctx, task_name.c_str(), task_param.c_str());

    return gen_result(ret);
}

json::value ExecAgentBase::ctx_run_recognizer(const json::value& cmd)
{
    auto ctx = get_sync_context(cmd);
    if (!ctx) {
        LogError << "sync context not found" << VAR(cmd);
        return invalid_json();
    }

    auto image_opt = cmd.find<std::string>("image");
    if (!image_opt) {
        LogError << "no image" << VAR(cmd);
        return invalid_json();
    }

    // TODO: image mode
    auto image = imread(*image_opt);
    if (image.empty()) {
        LogError << "image not found or empty" << VAR(*image_opt) << VAR(cmd);
        return invalid_json();
    }

    std::string task_name = cmd.get("task_name", std::string());
    if (task_name.empty()) {
        LogError << "task name empty" << VAR(cmd);
        return invalid_json();
    }

    std::string task_param = cmd.get("task_param", std::string());
    if (task_param.empty()) {
        task_param = json::object().to_string();
    }

    auto image_buff = MaaCreateImageBuffer();
    MaaSetImageRawData(image_buff, image.data, image.cols, image.rows, image.type());
    OnScopeLeave([&]() { MaaDestroyImageBuffer(image_buff); });

    auto out_box_buff = MaaCreateRectBuffer();
    OnScopeLeave([&]() { MaaDestroyRectBuffer(out_box_buff); });
    auto out_detail_buff = MaaCreateStringBuffer();
    OnScopeLeave([&]() { MaaDestroyStringBuffer(out_detail_buff); });

    bool ret = MaaSyncContextRunRecognizer(ctx, image_buff, task_name.c_str(), task_param.c_str(), out_box_buff,
                                           out_detail_buff);

    json::value ret_obj = gen_result(ret);
    if (!ret) {
        return ret_obj;
    }

    cv::Rect out_box { MaaGetRectX(out_box_buff), MaaGetRectY(out_box_buff), MaaGetRectW(out_box_buff),
                       MaaGetRectH(out_box_buff) };
    std::string out_detail = MaaGetString(out_detail_buff);

    ret_obj |= { { "out_box", json::array { out_box.x, out_box.y, out_box.width, out_box.height } },
                 { "out_detail", json::parse(out_detail).value_or(out_detail) } };
    return ret_obj;
}

json::value ExecAgentBase::ctx_run_action(const json::value& cmd)
{
    auto ctx = get_sync_context(cmd);
    if (!ctx) {
        LogError << "sync context not found" << VAR(cmd);
        return invalid_json();
    }

    std::string task_name = cmd.get("task_name", std::string());
    if (task_name.empty()) {
        LogError << "task name empty" << VAR(cmd);
        return invalid_json();
    }

    std::string task_param = cmd.get("task_param", std::string());
    if (task_param.empty()) {
        task_param = json::object().to_string();
    }

    auto cur_box_opt = cmd.find<json::array>("cur_box");
    if (!cur_box_opt || cur_box_opt->size() != 4 || !cur_box_opt->all<int>()) {
        LogError << "no cur box" << VAR(cmd);
        return invalid_json();
    }
    auto& j_cur_box = *cur_box_opt;
    auto cur_box_buff = MaaCreateRectBuffer();
    OnScopeLeave([&]() { MaaDestroyRectBuffer(cur_box_buff); });
    MaaSetRect(cur_box_buff, j_cur_box[0].as<int>(), j_cur_box[1].as<int>(), j_cur_box[2].as<int>(),
               j_cur_box[3].as<int>());

    auto cur_rec_detail_opt = cmd.find("cur_rec_detail");
    if (!cur_rec_detail_opt) {
        LogError << "no cur rec detail" << VAR(cmd);
        return invalid_json();
    }
    std::string str_cur_rec_detail =
        cur_rec_detail_opt->is_string() ? cur_rec_detail_opt->as_string() : cur_rec_detail_opt->to_string();

    bool ret =
        MaaSyncContextRunAction(ctx, task_name.c_str(), task_param.c_str(), cur_box_buff, str_cur_rec_detail.c_str());

    return gen_result(ret);
}

json::value ExecAgentBase::ctx_click(const json::value& cmd)
{
    auto ctx = get_sync_context(cmd);
    if (!ctx) {
        LogError << "sync context not found" << VAR(cmd);
        return invalid_json();
    }

    auto x_opt = cmd.find<int>("x");
    if (!x_opt) {
        LogError << "no x" << VAR(cmd);
        return invalid_json();
    }

    auto y_opt = cmd.find<int>("y");
    if (!y_opt) {
        LogError << "no y" << VAR(cmd);
        return invalid_json();
    }

    bool ret = MaaSyncContextClick(ctx, *x_opt, *y_opt);

    return gen_result(ret);
}

json::value ExecAgentBase::ctx_swipe(const json::value& cmd)
{
    auto ctx = get_sync_context(cmd);
    if (!ctx) {
        LogError << "sync context not found" << VAR(cmd);
        return invalid_json();
    }

    auto x1_opt = cmd.find<int>("x1");
    if (!x1_opt) {
        LogError << "no x1" << VAR(cmd);
        return invalid_json();
    }

    auto y1_opt = cmd.find<int>("y1");
    if (!y1_opt) {
        LogError << "no y1" << VAR(cmd);
        return invalid_json();
    }

    auto x2_opt = cmd.find<int>("x2");
    if (!x2_opt) {
        LogError << "no x2" << VAR(cmd);
        return invalid_json();
    }

    auto y2_opt = cmd.find<int>("y2");
    if (!y2_opt) {
        LogError << "no y2" << VAR(cmd);
        return invalid_json();
    }

    auto duration_opt = cmd.find<int>("duration");
    if (!duration_opt) {
        LogError << "no duration" << VAR(cmd);
        return invalid_json();
    }

    bool ret = MaaSyncContextSwipe(ctx, *x1_opt, *y1_opt, *x2_opt, *y2_opt, *duration_opt);

    return gen_result(ret);
}

json::value ExecAgentBase::ctx_press_key(const json::value& cmd)
{
    auto ctx = get_sync_context(cmd);
    if (!ctx) {
        LogError << "sync context not found" << VAR(cmd);
        return invalid_json();
    }

    auto keycode_opt = cmd.find<int>("keycode");
    if (!keycode_opt) {
        LogError << "no keycode" << VAR(cmd);
        return invalid_json();
    }

    bool ret = MaaSyncContextPressKey(ctx, *keycode_opt);

    return gen_result(ret);
}

json::value ExecAgentBase::ctx_touch_down(const json::value& cmd)
{
    auto ctx = get_sync_context(cmd);
    if (!ctx) {
        LogError << "sync context not found" << VAR(cmd);
        return invalid_json();
    }

    auto contact_opt = cmd.find<int>("contact");
    if (!contact_opt) {
        LogError << "no contact" << VAR(cmd);
        return invalid_json();
    }

    auto x_opt = cmd.find<int>("x");
    if (!x_opt) {
        LogError << "no x" << VAR(cmd);
        return invalid_json();
    }

    auto y_opt = cmd.find<int>("y");
    if (!y_opt) {
        LogError << "no y" << VAR(cmd);
        return invalid_json();
    }

    auto pressure_opt = cmd.find<int>("pressure");
    if (!pressure_opt) {
        LogError << "no pressure" << VAR(cmd);
        return invalid_json();
    }

    bool ret = MaaSyncContextTouchDown(ctx, *contact_opt, *x_opt, *y_opt, *pressure_opt);

    return gen_result(ret);
}

json::value ExecAgentBase::ctx_touch_move(const json::value& cmd)
{
    auto ctx = get_sync_context(cmd);
    if (!ctx) {
        LogError << "sync context not found" << VAR(cmd);
        return invalid_json();
    }
    auto contact_opt = cmd.find<int>("contact");
    if (!contact_opt) {
        LogError << "no contact" << VAR(cmd);
        return invalid_json();
    }

    auto x_opt = cmd.find<int>("x");
    if (!x_opt) {
        LogError << "no x" << VAR(cmd);
        return invalid_json();
    }

    auto y_opt = cmd.find<int>("y");
    if (!y_opt) {
        LogError << "no y" << VAR(cmd);
        return invalid_json();
    }

    auto pressure_opt = cmd.find<int>("pressure");
    if (!pressure_opt) {
        LogError << "no pressure" << VAR(cmd);
        return invalid_json();
    }

    bool ret = MaaSyncContextTouchMove(ctx, *contact_opt, *x_opt, *y_opt, *pressure_opt);

    return gen_result(ret);
}

json::value ExecAgentBase::ctx_touch_up(const json::value& cmd)
{
    auto ctx = get_sync_context(cmd);
    if (!ctx) {
        LogError << "sync context not found" << VAR(cmd);
        return invalid_json();
    }
    auto contact_opt = cmd.find<int>("contact");
    if (!contact_opt) {
        LogError << "no contact" << VAR(cmd);
        return invalid_json();
    }

    bool ret = MaaSyncContextTouchUp(ctx, *contact_opt);

    return gen_result(ret);
}

json::value ExecAgentBase::ctx_screencap(const json::value& cmd)
{
    auto ctx = get_sync_context(cmd);
    if (!ctx) {
        LogError << "sync context not found";
        return invalid_json();
    }

    auto image_buff = MaaCreateImageBuffer();
    OnScopeLeave([&]() { MaaDestroyImageBuffer(image_buff); });

    bool ret = MaaSyncContextScreencap(ctx, image_buff);

    auto ret_obj = gen_result(ret);
    if (!ret) {
        return ret_obj;
    }

    void* raw_data = MaaGetImageRawData(image_buff);
    int32_t width = MaaGetImageWidth(image_buff);
    int32_t height = MaaGetImageHeight(image_buff);
    int32_t type = MaaGetImageType(image_buff);
    cv::Mat image(height, width, type, raw_data);
    if (image.empty()) {
        LogError << "image empty";
        return invalid_json();
    }

    std::string image_arg = arg_cvt_.image_to_arg(image, ImageTransferMode::FileIO);
    ret_obj |= { { "out_image", image_arg } };
    return ret_obj;
}

json::value ExecAgentBase::ctx_get_task_result(const json::value& cmd)
{
    auto ctx = get_sync_context(cmd);
    if (!ctx) {
        LogError << "sync context not found";
        return invalid_json();
    }

    std::string task_name = cmd.get("task_name", std::string());
    if (task_name.empty()) {
        LogError << "task name empty" << VAR(cmd);
        return invalid_json();
    }

    auto out_task_result_buff = MaaCreateStringBuffer();
    OnScopeLeave([&]() { MaaDestroyStringBuffer(out_task_result_buff); });

    bool ret = MaaSyncContextGetTaskResult(ctx, task_name.c_str(), out_task_result_buff);

    auto ret_obj = gen_result(ret);
    if (!ret) {
        return ret_obj;
    }

    std::string out_task_result = MaaGetString(out_task_result_buff);
    ret_obj |= { { "out_task_result", json::parse(out_task_result).value_or(out_task_result) } };
    return ret_obj;
}

MaaSyncContextHandle ExecAgentBase::get_sync_context(const json::value& cmd)
{
    auto str_opt = cmd.find<std::string>("sync_context");
    if (!str_opt) {
        LogError << "no sync context" << VAR(cmd);
        return nullptr;
    }

    auto ctx = arg_cvt_.arg_to_sync_context(*str_opt);
    if (!ctx) {
        LogError << "sync context not found" << VAR(*str_opt) << VAR(cmd);
        return nullptr;
    }

    return ctx;
}

json::value ExecAgentBase::invalid_json()
{
    return { { "error", "invalid json" } };
}

json::value ExecAgentBase::gen_result(bool success)
{
    return { { "return", success } };
}

MAA_TOOLKIT_NS_END
