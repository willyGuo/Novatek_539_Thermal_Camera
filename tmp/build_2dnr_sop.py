from pathlib import Path
from docx import Document
from docx.shared import Cm, Pt, RGBColor
from docx.oxml import OxmlElement
from docx.oxml.ns import qn
from docx.enum.text import WD_ALIGN_PARAGRAPH
from docx.enum.table import WD_TABLE_ALIGNMENT, WD_CELL_VERTICAL_ALIGNMENT

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'output' / 'documents'
OUT.mkdir(parents=True, exist_ok=True)
doc = Document()
s = doc.sections[0]
s.page_height, s.page_width = Cm(29.7), Cm(21)
s.top_margin, s.bottom_margin = Cm(1.7), Cm(1.6)
s.left_margin = s.right_margin = Cm(1.65)
s.header_distance = s.footer_distance = Cm(.7)
for name in ['Normal','Title','Subtitle','Heading 1','Heading 2']:
    st = doc.styles[name]
    st.font.name = 'Microsoft JhengHei'
    st.element.get_or_add_rPr().rFonts.set(qn('w:eastAsia'), 'Microsoft JhengHei')
    st.font.color.rgb = RGBColor(0,0,0)
doc.styles['Normal'].font.size = Pt(10.5)
doc.styles['Normal'].font.bold = False
doc.styles['Normal'].paragraph_format.line_spacing = 1.15
doc.styles['Normal'].paragraph_format.space_after = Pt(5)
doc.styles['Title'].font.size = Pt(24)
doc.styles['Heading 1'].font.size = Pt(17)
doc.styles['Heading 2'].font.size = Pt(12)
for st in doc.styles:
    for border in list(st.element.iter(qn('w:pBdr'))):
        border.getparent().remove(border)
for name in ['Heading 1','Heading 2']:
    doc.styles[name].paragraph_format.space_before = Pt(9)
    doc.styles[name].paragraph_format.space_after = Pt(5)

def p(text='', bold=False):
    x=doc.add_paragraph()
    x.add_run(text).bold=bold
    return x
def h(text, level=1): doc.add_heading(text,level)
def page(title):
    doc.add_page_break()
    h(title)
def table(headers, rows, widths, fs=9.5):
    t=doc.add_table(rows=1, cols=len(headers))
    t.alignment=WD_TABLE_ALIGNMENT.CENTER
    t.autofit=False
    for c,w in zip(t.columns,widths): c.width=Cm(w)
    for i,v in enumerate(headers): t.rows[0].cells[i].text=v
    for row in rows:
        cells=t.add_row().cells
        for i,v in enumerate(row): cells[i].text=str(v)
    for ri,row in enumerate(t.rows):
        pr=row._tr.get_or_add_trPr()
        nosplit=OxmlElement('w:cantSplit'); pr.append(nosplit)
        if ri==0:
            repeat=OxmlElement('w:tblHeader'); pr.append(repeat)
        for ci,cell in enumerate(row.cells):
            cell.width=Cm(widths[ci])
            cell.vertical_alignment=WD_CELL_VERTICAL_ALIGNMENT.CENTER
            cp=cell._tc.get_or_add_tcPr()
            borders=OxmlElement('w:tcBorders')
            for edge in ['top','left','bottom','right']:
                el=OxmlElement('w:'+edge)
                el.set(qn('w:val'),'single'); el.set(qn('w:sz'),'4'); el.set(qn('w:color'),'D9D9D9')
                borders.append(el)
            cp.append(borders)
            margins=OxmlElement('w:tcMar')
            for edge in ['top','left','bottom','right']:
                el=OxmlElement('w:'+edge); el.set(qn('w:w'),'75'); el.set(qn('w:type'),'dxa'); margins.append(el)
            cp.append(margins)
            shade=OxmlElement('w:shd'); shade.set(qn('w:fill'),'DCE6F1' if ri==0 else ('F5F7FA' if ri%2==0 else 'FFFFFF')); cp.append(shade)
            for para in cell.paragraphs:
                para.paragraph_format.space_after=Pt(1)
                para.paragraph_format.line_spacing=1.08
                for run in para.runs:
                    run.font.size=Pt(fs)
                    run.bold=(ri==0)
    spacer=doc.add_paragraph()
    spacer.paragraph_format.space_after=Pt(0)
    spacer.paragraph_format.line_spacing=Pt(3)
    spacer.add_run().font.size=Pt(1)
    return t

title=doc.add_paragraph('Novatek 2DNR 參數調整 SOP', 'Title')
title.runs[0].font.size=Pt(24)
title.runs[0].font.bold=True
title.runs[0].font.color.rgb=RGBColor(0,0,0)
p('依 ISP Tool 畫面操作與學習', True)
p('版本 1.0　｜　日期 2026 年 11 月 12 日')
p('本 SOP 用於逐項學習 2DNR 的降噪與細節取捨。先調整中央 Filter A／B，再調整細節還原；Outlier、G Balance、LCA 與 DBCS 依問題選用。每次只變更一項，保存參數與同場景影像後再比較。')
h('1 畫面位置與調整順序')
table(['畫面位置','功能','本 SOP 的使用順序'],[
('中央上方及右上方','Filter A／Filter B','先調 TH，再調亮度 LUT'),
('中央 Filter Control','A／B 混合與細節還原','blend → clamp_th → clamp_mul'),
('左上 Outlier','動態亮暗壞點補償','有異常亮暗點時處理'),
('左下 G Balance','Bayer 綠色通道平衡','有對應紋路且路徑適用時處理'),
('右下 LCA 及中央下方 DBCS','亮度大範圍雜訊及色噪處理','完成主要 2D Filter 後再評估')],[4.1,5.0,8.6])
h('2 截圖中的主要起始值')
table(['欄位','截圖值','讀值重點'],[
('Fast TH／TH B／Lut／Lut B','皆為 -1','快速產表控制的初次連線預設顯示，不等於關閉'),
('filter_blend_w','0','位於偏向 Filter A 的端點'),
('filter_clamp_th','0','位於細節還原較少的一端'),
('filter_clamp_mul','128','細節還原倍率 1 倍，不是降噪倍率'),
('outl_ord_blend_w','0','採用 Order 結果'),
('outl_bright_th／outl_dark_th','顯示節點皆為 4095','高偵測門檻；仍需確認完整表格及模組開關')],[7.1,2.5,8.1])
p('注意：截圖未顯示全部節點及各模組 Enable。請以實際連線讀回的完整參數作為基準，不要直接把截圖數值視為目前硬體設定。', True)

page('3 調整前準備')
h('3 1 保存可回退的基準',2)
p('讀回目前設定，保存為 2DNR_B00_baseline.cfg，並保存對應的校正 BIN 與載入路徑。記錄 Sensor、SoC、ISP Tool 版本、解析度、輸入格式、曝光或增益，以及各模組 Enable。')
p('確認讀寫沒有 CFG Error，套用後重新讀回主要參數。工具顯示完成訊息，仍應搭配讀回值與畫面確認。')
h('3 2 確認正在修改的參數會生效',2)
p('確認 NR 的 filter_enable 已開啟，先使用手動模式調試。畫面上顯示 IQ_NR_TUNE_PARAM 是參數結構選擇，不足以證明已切到 Manual。若仍使用 Auto，ISO／Enhance ISO 的內插可能改變實際生效值。')
p('第一輪固定 AGC、3DNR、Edge 與 Post Sharpen 設定。若銳化干擾判讀，可另存一個關閉銳化的測試基準；後續所有候選均使用相同條件。')
h('3 3 固定測試場景',2)
table(['區域','擺放或選擇方式','每次都要觀察'],[
('平坦區','牆面或均勻溫度區','顆粒、斑駁、固定紋路是否下降'),
('細節區','文字、細線、物體邊界','細線是否消失，邊界是否變寬'),
('小型熱目標','面積小且可重複定位的熱目標','是否被當成噪聲或壞點消除'),
('亮度區間','同畫面保留暗、中、亮區','某一亮度區是否特別模糊或特別吵'),
('移動區','以相近速度移動手部或目標','拖影、殘像、輪廓破碎')],[2.5,6.4,8.8])
h('3 4 統一比較方式',2)
p('固定 ROI、顯示倍率、AGC 與儲存格式，每組保存相同長度的影像。建議另存無疊字影像，檔名與 CFG 使用同一組編號。相機溫度、場景或 AGC 明顯改變時，重新建立基準。')
p('平坦 ROI 的空間標準差可輔助比較顆粒；但真實溫度梯度及固定紋路也會影響數值。必須同時確認細節與小目標，不能只以標準差下降判定成功。')

page('4 主要 2D Filter 操作')
p('本頁依原廠第 74～75 頁的順序調試。以下的實驗階梯屬學習用安排，不是原廠最佳值或硬體驗證結果。')
h('步驟 1 調整 Filter A 的 TH',2)
p('操作位置：中央 Filter A → Fast TH。先維持 filter_blend_w = 0，固定 LUT 與 clamp，讓比較集中在 Filter A。')
p('先保存原有完整 filter_th。Fast TH 顯示 -1 時，選擇工具允許的有效設定，產生第一組 TH；讀回並另存。之後每次只提高一小格，直到平坦區改善，或細節首次明顯受損。')
p('判定：平坦區更乾淨，細線與小目標仍可辨識才保留；若細節受損，回退前一組。Fast TH 越大，抑噪能力越強。filter_th 是 6 個 threshold，數值範圍 0～1023。')
h('步驟 2 調整亮度 LUT',2)
p('操作位置：中央 Filter A → Fast Lut。固定步驟 1 選定的 TH，只改 Fast Lut；每次變更後保存完整 filter_lut。')
p('Fast Lut 越大，依亮度提升抑噪的能力越強。先用快速產表建立形狀，再比較暗、中、亮區；只有局部亮度區需要處理時，才微調相應節點。filter_lut 有 17 個節點，範圍 0～1023，並非 Gamma 或輸出亮度曲線。')
h('步驟 3 建立 Filter B 並調混合比例',2)
p('操作位置：右上 Filter B，以及中央 Filter Control → filter_blend_w。Filter A 偏向保留邊緣紋理，Filter B 偏向較強去噪。')
p('原廠建議：Fast TH 的 A:B 約 1:3，Fast Lut 的 A、B 相等。這是快速產表控制值的建議，不能直接解讀為每一個 TH 節點乘 3。使用 A／B 的 Sync 功能後，仍需檢查兩邊實際產生的表格。')
p('建立 A、B 後固定兩套表格，依序試 filter_blend_w = 0 → 2 → 4 → 8，必要時在相鄰值間細調。範圍為 0～15；越大越偏 B。只改 B、卻一直保持 blend = 0，不適合用來評估 B 的效果。')
p('注意：上方 Sync IQ_NR_EXT_TUNE_PARAM 涉及擴充參數組，不應直接當成 A／B 的 Sync。若使用 Aided 分區模式，須另外確認所選分區與 EXT 對應。', True)

page('5 細節還原與回退')
h('步驟 4 調整 filter_clamp_th',2)
p('操作位置：中央 Filter Control → filter_clamp_th。固定已選定的 TH、LUT、blend 及 clamp_mul，僅比較 clamp_th。')
table(['練習值','預期方向','主要判定'],[
('0','較少細節還原','觀察細節是否被過度抹除'),
('4','增加部分細節還原','細線能否恢復而平坦區仍乾淨'),
('16','還原更多細節及可能的噪聲','顆粒回升時與前一組比較')],[2.5,6.1,9.1])
p('0、4、16 是原廠第 75 頁的示意值，可作為練習比較點。實際可接受值由場景決定；此參數範圍是 0～4095。越大並不是降噪越強。')
h('步驟 5 調整 filter_clamp_mul',2)
p('操作位置：中央 Filter Control → filter_clamp_mul。先固定 clamp_th，選一個已能看出細節還原差異的設定，再比較倍率；若 clamp_th = 0 時差異不明顯，先完成上一個步驟。')
table(['練習值','細節還原倍率','觀察方向'],[
('64','0.5 倍','細節及被還原的噪聲較少'),
('96','0.75 倍','在細節與顆粒間折衷'),
('128','1 倍','細節還原較多，也可能加回顆粒')],[2.5,4.0,11.2])
p('倍率基準是 128 = 1 倍，範圍 0～255。倍率指細節還原，不代表最終像素值、噪聲或標準差按相同比例變化。')
h('步驟 6 完成動態檢查並選版',2)
p('用相同速度移動目標，檢查拖影、殘像、輪廓變寬與小目標消失。若出現上述現象，恢復前一組 CFG，重新讀回，再用同場景確認。LCA 與 3DNR 的交互影響須另外排查。')
p('保留條件：平坦區噪聲改善、目標細節可接受、移動表現可接受，且沒有新增明顯斑駁、光暈或不穩定。完成候選選擇後，再載入該 CFG 並重複一次驗證，確認保存與讀回一致。')

page('6 Outlier 與 G Balance 操作')
h('6 1 有異常亮暗點時調整 Outlier',2)
p('先確認 outl_enable。不要用一般顆粒改善，取代壞點定位與小目標保留檢查。原廠第 68～70 頁將此功能分為 Order 與 Outlier 兩種算法。')
table(['欄位','操作方向及判定'],[
('outl_ord_blend_w','0 採 Order，255 採 Outlier。截圖為 0，先調 Order 的保護門檻。'),
('outl_ord_protect_th','降低可加強細小結構保護，減少誤補償；範圍 0～1023。比較小型熱目標是否仍存在。'),
('outl_avg_mode','原廠建議 Disable／False，採方向性補償，減少錯誤補償。'),
('outl_sel','8_NODE 較低敏感度；存在相同通道連續壞點時才評估 7_NODE，並檢查細節誤刪。'),
('outl_bright_th','降低會更容易偵測亮壞點；依亮度調整，避免真實小熱點消失。範圍 0～4095。'),
('outl_dark_th','降低會更容易偵測暗壞點；檢查暗細節是否被誤補償。範圍 0～4095。')],[5.0,12.7])
p('實作順序：保存基準 → 選定算法 → 一次只改一項門檻 → 比較固定壞點及真實小目標 → 保留或回退。偏 Order 時，不要把 Outlier 亮暗門檻當成主要控制。')
h('6 2 有對應 Bayer 紋路時評估 G Balance',2)
p('先確認 gbal_enable 及輸入路徑適用。此功能處理綠色通道不平衡造成的迷宮狀紋路；單色熱影像不應直接把它當通用降噪控制。')
table(['欄位','調整方向'],[
('gbal_diff_th_str','提高會加強補償，也可能模糊邊緣及細節。'),
('gbal_edge_protect_th','降低會更容易判定邊緣並抑制補償，保護細節。'),
('gbal_str_luma_low_bnd','提高可加強低亮度區的補償。'),
('gbal_edge_luma_low_bnd','降低可加強暗部邊緣保護；提高則放寬暗部保護。')],[6.2,11.5])

page('7 LCA 與 DBCS 操作')
p('LCA 表格三欄順序為：靜止邊緣區、靜止平坦區、移動區。先確認 lca_enable、dbcs_enable，以及實際訊號包含亮度或色彩資訊。')
h('7 1 先確認是否需要 LCA Y',2)
p('原廠第 77 頁通常不建議一般串流開啟 LCA Y，主要在無法使用 3DNR 的情況下考慮。LCA 結果可能作為 3DNR 參考幀，過強會增加拖影；應先完成主要 2D Filter。')
table(['欄位','操作方式'],[
('lca_edge_th','使用 LCA_EDGE 除錯畫面調整。過低可能把平坦區噪聲判為邊緣；過高可能把真實邊緣判為平坦區而模糊。'),
('lca_y_filter_level','依原廠 LCA Y 流程，先以 0 建立基準，並同時將 Y coring gain／cutoff 設為 0。'),
('lca_y_coring_gain','需要亮度降噪時逐步增加；邊緣或細節明顯降低即回退。範圍 0～32。'),
('lca_y_coring_cutoff','提高可減弱 coring gain 效果。用於 gain 加減 1 都無法平衡時的細調，範圍 0～63。')],[5.5,12.2])
h('7 2 有色噪時再調 LCA C',2)
table(['欄位','作用及判定'],[
('lca_c_filter_level','提高會加強色噪抑制，但可能掉色、溢色；範圍 0～255。'),
('lca_c_coring_gain','提高會加強處理，也可能影響顏色；範圍 0～32。'),
('lca_c_coring_cutoff','提高可緩和效果；移動區出現色彩拖影時可評估，範圍 0～63。')],[5.5,12.2])
p('原廠以強設定展示效果後再逐步減弱；實際調試需先保存基準，確認掉色、溢色及移動表現均可接受。純灰階的亮度顆粒不應優先靠 LCA C 處理。')
h('7 3 需要暗部色彩抑制時調 DBCS',2)
p('dbcs_step_y 與 dbcs_step_c 皆為 0～3。step_y 越大，納入處理的亮度範圍越廣；step_c 越大，色彩範圍越廣。此功能會將符合條件的像素拉向灰階，不是單純提高亮度降噪強度。')
p('截圖兩者皆為 2。若要測試，先固定其中一項，只改另一項並檢查暗部掉色；處理範圍過大或移動顏色異常時回退。原廠提醒 DBCS 可能影響 3DNR 參考幀，需一併確認。')

page('8 操作紀錄與參考資料')
h('8 1 每次測試填寫',2)
p('日期與操作者：________________　Sensor／SoC：________________')
p('ISP Tool 版本：________________　輸入格式與解析度：________________')
p('基準 CFG：________________　校正 BIN 路徑：________________')
p('AGC／3DNR／Edge／Post Sharpen 固定設定：________________________')
table(['編號','單一變更與舊值新值','影像或 CFG 檔名','結果及回退'],[
('B00','原始讀回基準','________________','________________'),
('T01','________________','________________','________________'),
('T02','________________','________________','________________'),
('T03','________________','________________','________________'),
('T04','________________','________________','________________'),
('T05','________________','________________','________________')],[1.4,6.0,5.5,4.8])
p('每組結果至少記錄：平坦區顆粒、細線／小熱點、移動拖影、是否讀回一致。完整 TH／LUT 應保存在 CFG；不要只記 Fast 值。')
h('8 2 常見現象與下一個檢查方向',2)
table(['現象','檢查方向'],[
('只調 Filter B 卻看不出差異','確認 filter_enable、Manual、生效表格，以及 blend 是否仍為 0。'),
('平坦區乾淨但細節消失','減弱 TH／LUT 或降低 B 比例；再小幅增加 clamp 細節還原。'),
('增加 clamp 後顆粒回來','降低 clamp_th 或 clamp_mul，保留前一個可接受版本。'),
('出現明顯拖影','回退最近設定，分別檢查 LCA 與 3DNR 的參考幀交互影響。'),
('重載後效果不同','檢查 CFG／BIN、模組開關、模式、讀回值與場景條件。')],[5.3,12.4])
h('8 3 參考資料',2)
ref=p('Novatek ISP Tuning Guide：檔名 NT98538_ISP_Tuning_Guide_tc.pdf，內頁標題 NT98539A ISP Tuning Guide。模式 p20；Outlier／G Balance p68～73；2D Filter p74～76；LCA／DBCS p77～83。')
for r in ref.runs: r.font.size=Pt(9)
ref=p('SDK：ns2302_linux_sdk-ty640_parallel/code/hdal/vendor/isp/drivers/include/iq_alg.h，第 283～335 行，含參數範圍與模組開關。')
for r in ref.runs: r.font.size=Pt(9)
ref=p('blend 與 clamp_mul 階梯為練習值。最終設定須完成板端同場景靜態及動態驗證。', True)
for r in ref.runs: r.font.size=Pt(9)

footer=s.footer.paragraphs[0]
footer.alignment=WD_ALIGN_PARAGRAPH.RIGHT
footer.add_run('2DNR 調整 SOP　｜　')
field=OxmlElement('w:fldSimple'); field.set(qn('w:instr'),'PAGE'); footer._p.append(field)
for r in footer.runs: r.font.size=Pt(9)
doc.core_properties.title='Novatek 2DNR 參數調整 SOP'
doc.core_properties.subject='ISP Tool 2DNR 學習與操作流程'
doc.core_properties.author=''
target=OUT/'Novatek_2DNR_參數調整_SOP_v1.0.docx'
doc.save(target)
print(target)
