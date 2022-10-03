#include "DepotImageAnalyzer.h"

#include "Utils/NoWarningCV.h"

#include "General/MatchImageAnalyzer.h"
#include "General/OcrWithPreprocessImageAnalyzer.h"
#include "Resource/ItemConfiger.h"
#include "TaskData.h"
#include "Utils/AsstUtils.hpp"
#include "Utils/Logger.hpp"

bool asst::DepotImageAnalyzer::analyze()
{
    LogTraceFunction;

    m_all_items_roi.clear();
    m_result.clear();

    // 因为模板素材的尺寸与实际截图中素材尺寸不符，所以这里先对原图进行一下缩放
    resize();
    bool ret = analyze_base_rect();
    if (!ret) {
        return false;
    }

    ret = analyze_all_items();

#ifdef ASST_DEBUG
    m_image_draw = m_image_draw_resized;
#endif
    save_img("debug/depot/");
    return ret;
}

void asst::DepotImageAnalyzer::set_match_begin_pos(size_t pos) noexcept
{
    m_match_begin_pos = pos;
}

size_t asst::DepotImageAnalyzer::get_match_begin_pos() const noexcept
{
    return m_match_begin_pos;
}

void asst::DepotImageAnalyzer::resize()
{
    LogTraceFunction;

    m_resized_rect = Task.get("DepotMatchData")->roi;
    cv::Size d_size(m_resized_rect.width, m_resized_rect.height);
    cv::resize(m_image, m_image_resized, d_size, 0, 0, cv::INTER_AREA);
#ifdef ASST_DEBUG
    cv::resize(m_image_draw, m_image_draw_resized, d_size, 0, 0, cv::INTER_AREA);
#endif
}

bool asst::DepotImageAnalyzer::analyze_base_rect()
{
    LogTraceFunction;

    Rect base_roi = Task.get("DepotBaseRect")->roi;
    ItemInfo base_item_info;
    size_t pos = match_item(base_roi, base_item_info, 0ULL, false);
    if (pos == NPos) {
        return false;
    }
    const auto& base_rect = base_item_info.rect;
    const auto& [base_x, base_y, base_w, base_h] = base_rect;
#ifdef ASST_DEBUG
    cv::rectangle(m_image_draw_resized, make_rect<cv::Rect>(base_rect), cv::Scalar(0, 0, 255), 2);
#endif

    // 每个材料之间有间隔，这里 高度 * 2 的 roi, 一定有一个完整的材料 + 一个不完整的材料。
    // 所以识别结果只会有一个（除非这个间隔非常大）
    const Rect vertical_rect(base_x, base_y + base_h, base_w, base_h * 2);
    ItemInfo vertical_item_info;
    pos = match_item(vertical_rect, vertical_item_info, pos + 1);

#ifdef ASST_DEBUG
    cv::rectangle(m_image_draw_resized, make_rect<cv::Rect>(vertical_item_info.rect), cv::Scalar(0, 0, 255), 2);
#endif

    // 宽度 * 2，同上
    const Rect horizontal_roi(base_x + base_w, base_y, base_w * 2, base_h);
    ItemInfo horizontal_item_info;
    pos = match_item(horizontal_roi, horizontal_item_info, pos + 1);

#ifdef ASST_DEBUG
    cv::rectangle(m_image_draw_resized, make_rect<cv::Rect>(horizontal_item_info.rect), cv::Scalar(0, 0, 255), 2);
#endif

    const int horizontal_spacing = horizontal_item_info.rect.x - (base_x + base_w);
    const int vertical_spacing = vertical_item_info.rect.y - (base_y + base_h);

    for (int x = base_x; x <= m_image_resized.cols; x += (base_w + horizontal_spacing)) {
        for (int y = base_y; y <= m_image_resized.rows; y += (base_h + vertical_spacing)) {
            Rect item_roi = Rect(x, y, base_w, base_h);
            if (!m_resized_rect.include(item_roi)) {
                continue;
            }
            m_all_items_roi.emplace_back(item_roi);
#ifdef ASST_DEBUG
            cv::rectangle(m_image_draw_resized, make_rect<cv::Rect>(item_roi), cv::Scalar(0, 255, 0), 2);
#endif
        }
    }

    return !m_all_items_roi.empty();
}

bool asst::DepotImageAnalyzer::analyze_all_items()
{
    LogTraceFunction;

    for (const Rect& roi : m_all_items_roi) {
        if (check_roi_empty(roi)) { // roi 是竖着有序的
            break;
        }
        ItemInfo info;
        size_t cur_pos = match_item(roi, info, m_match_begin_pos);
        if (cur_pos == NPos) {
            break;
        }
        std::string item_id = info.item_id;

        m_match_begin_pos = cur_pos + 1;
        info.quantity = match_quantity(info.rect);
        info.item_name = ItemData.get_item_name(item_id);
#ifdef ASST_DEBUG
        cv::putText(m_image_draw_resized, item_id, cv::Point(roi.x, roi.y - 10), cv::FONT_HERSHEY_SIMPLEX, 0.5,
                    cv::Scalar(0, 0, 255), 2);
        cv::putText(m_image_draw_resized, std::to_string(info.quantity), cv::Point(roi.x, roi.y + 10),
                    cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(0, 0, 255), 2);
#endif
        if (item_id.empty() || info.quantity == 0) {
            Log.error(__FUNCTION__, item_id, info.item_name, " quantity is zero");
            continue;
        }
        info.rect = resize_rect_to_raw_size(info.rect);
        m_result.emplace(std::move(item_id), std::move(info));
    }
#ifdef ASST_DEBUG
    cv::Mat hsv;
    cv::cvtColor(m_image_resized, hsv, cv::COLOR_BGR2HSV);
#endif

    return !m_result.empty();
}

bool asst::DepotImageAnalyzer::check_roi_empty(const Rect& roi)
{
    // TODO
    std::ignore = roi;
    return false;
}

size_t asst::DepotImageAnalyzer::match_item(const Rect& roi, /* out */ ItemInfo& item_info, size_t begin_index,
                                            bool with_enlarge)
{
    LogTraceFunction;

    const auto& all_items = ItemData.get_ordered_material_item_id();

    MatchImageAnalyzer analyzer(m_image_resized);
    analyzer.set_task_info("DepotMatchData");
    // spacing 有时候算的差一个像素，干脆把 roi 扩大一点好了
    Rect enlarged_roi = roi;
    if (with_enlarge) {
        enlarged_roi = Rect(roi.x - 20, roi.y - 5, roi.width + 40, roi.height + 10);
    }
    analyzer.set_roi(enlarged_roi);

    MatchRect matched;
    std::string matched_item_id;
    size_t matched_index = NPos;
    for (size_t index = begin_index, extra_count = 0; index < all_items.size(); ++index) {
        const std::string& item_id = all_items.at(index);
        analyzer.set_templ_name(item_id);
        if (!analyzer.analyze()) {
            continue;
        }
        if (double score = analyzer.get_result().score; score >= matched.score) {
            matched = analyzer.get_result();
            matched_item_id = item_id;
            matched_index = index;
        }
        // 匹配到了任一结果后，再往后匹配几个。
        // 因为有些相邻的材料长得很像（同一种类的）
        constexpr size_t MaxExtraMatch = 8;
        if (matched_index != NPos && ++extra_count >= MaxExtraMatch) {
            break;
        }
    }
    Log.info("Item id:", matched_item_id);
    if (matched_item_id.empty()) {
        return NPos;
    }
    item_info.item_id = matched_item_id;
    item_info.rect = matched.rect;
    return matched_index;
}

int asst::DepotImageAnalyzer::match_quantity(const Rect& roi)
{
    auto task_ptr = Task.get<MatchTaskInfo>("DepotQuantity");

    Rect quantity_roi = roi.move(task_ptr->roi);
    cv::Mat quantity_img = m_image_resized(make_rect<cv::Rect>(quantity_roi));
    cv::Mat hsv;
    cv::cvtColor(quantity_img, hsv, cv::COLOR_BGR2HSV);

    const int h_lower = task_ptr->mask_range.first;
    const int h_upper = task_ptr->mask_range.second;
    const int s_lower = task_ptr->specific_rect.x;
    const int s_upper = task_ptr->specific_rect.y;
    const int v_lower = task_ptr->specific_rect.width;
    const int v_upper = task_ptr->specific_rect.height;

    cv::Mat bin;
    cv::inRange(hsv, cv::Scalar(h_lower, s_lower, v_lower), cv::Scalar(h_upper, s_upper, v_upper), bin);

    // split
    const int max_spacing = static_cast<int>(task_ptr->templ_threshold);
    const int bg_v_upper = static_cast<int>(task_ptr->special_threshold);
    std::vector<cv::Range> contours;
    int i_right = bin.cols - 1, i_left = 0;
    bool in = false;
    int spacing = 0;

    for (int i = bin.cols - 1; i >= 0; --i) {
        bool has_white = false;
        for (int j = 0; j < bin.rows; ++j) {
            if (bin.at<uchar>(j, i)) {
                has_white = true;
                break;
            }
        }
        if (in && !has_white) { // leave
            i_left = i;
            in = false;
            spacing = 0;
            contours.emplace_back(i_left, i_right + 1); // range 是前闭后开的
        }
        else if (!in && has_white) { // enter
            i_right = i;
            in = true;
        }
        else if (!in) {
            ++spacing;
            uchar bg_top_v = hsv.at<cv::Vec3b>(0, i)[2];
            uchar bg_btm_v = hsv.at<cv::Vec3b>(bin.rows - 1, i)[2];
            if (i_left != 0 && (spacing > max_spacing || bg_top_v > bg_v_upper || bg_btm_v > bg_v_upper)) {
                // filter out noise
                break;
            }
        }
    }

    cv::Rect y_bounding_rect;
    for (int i = 0; i < contours.size(); ++i) {
        if (y_bounding_rect.empty()) {
            auto temp = cv::boundingRect(bin(cv::Range::all(), contours.at(i)));
            if (temp.height > 10) {
                y_bounding_rect = temp;
            }
            else {
                continue;
            }
        }
        auto left_elem_rect = cv::boundingRect(bin(cv::Range::all(), contours.back()));
        if (std::abs(left_elem_rect.height - y_bounding_rect.height) > 2 ||
            std::abs(left_elem_rect.y - y_bounding_rect.y) > 1 || left_elem_rect.width > 12) {
            contours.pop_back();
        }
        else {
            break;
        }
    }
    if (contours.empty()) {
        return 0;
    }
    int far_left = contours.back().start;
    int far_right = contours.front().end;

    OcrWithPreprocessImageAnalyzer analyzer(m_image_resized);
    analyzer.set_task_info("NumberOcrReplace");
    Rect ocr_roi(quantity_roi.x + far_left, quantity_roi.y + y_bounding_rect.y, far_right - far_left,
                 y_bounding_rect.height);
    analyzer.set_roi(ocr_roi);
    analyzer.set_expansion(1);
    analyzer.set_threshold(task_ptr->mask_range.first, task_ptr->mask_range.second);

    if (!analyzer.analyze()) {
        return 0;
    }

    const auto& result = analyzer.get_result().front();

#ifdef ASST_DEBUG
    cv::rectangle(m_image_draw_resized, make_rect<cv::Rect>(result.rect), cv::Scalar(0, 0, 255));
    cv::putText(m_image_draw_resized, result.text, cv::Point(result.rect.x, result.rect.y - 5),
                cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(0, 255, 0), 2);
#endif

    std::string digit_str = result.text;
    int multiple = 1;
    if (size_t w_pos = digit_str.find("万"); w_pos != std::string::npos) {
        multiple = 10000;
        digit_str.erase(w_pos, digit_str.size());
    }
    // else if (size_t e_pos = digit_str.find("亿");
    //     e_pos != std::string::npos) {
    //     multiple = 100000000;
    //     digit_str.erase(e_pos, digit_str.size());
    // }

    if (digit_str.empty() ||
        !ranges::all_of(digit_str, [](const char& c) -> bool { return std::isdigit(c) || c == '.'; })) {
        return 0;
    }

    int quantity = static_cast<int>(std::stod(digit_str) * multiple);
    Log.info("Quantity:", quantity);
    return quantity;
}

asst::Rect asst::DepotImageAnalyzer::resize_rect_to_raw_size(const asst::Rect& rect)
{
    LogTraceFunction;

    m_resized_rect = Task.get("DepotMatchData")->roi;

    double kx = static_cast<double>(m_image.cols) / m_resized_rect.width;
    double ky = static_cast<double>(m_image.rows) / m_resized_rect.height;

    Rect raw_rect;
    raw_rect.x = static_cast<int>(kx * rect.x);
    raw_rect.y = static_cast<int>(ky * rect.y);
    raw_rect.width = static_cast<int>(kx * rect.width);
    raw_rect.height = static_cast<int>(ky * rect.height);

    return raw_rect;
}
