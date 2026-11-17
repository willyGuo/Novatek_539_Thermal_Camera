from pathlib import Path
from docx import Document
from docx.shared import Cm, Pt
from docx.enum.section import WD_SECTION_START, WD_ORIENT
from docx.enum.text import WD_ALIGN_PARAGRAPH
from docx.oxml.ns import qn

root=Path(__file__).resolve().parents[1]
src=root/'output/documents/Novatek_2DNR_參數調整_SOP_v1.0.docx'
dst=root/'output/documents/Novatek_2DNR_參數調整_SOP_v1.1_含截圖.docx'
doc=Document(src)
for para in doc.paragraphs:
    if para.text.startswith('版本 1.0'):
        for run in para.runs:
            run.text=run.text.replace('版本 1.0','版本 1.1')
sec=doc.add_section(WD_SECTION_START.NEW_PAGE)
sec.orientation=WD_ORIENT.LANDSCAPE
sec.page_width,sec.page_height=Cm(29.7),Cm(21)
sec.top_margin,sec.bottom_margin=Cm(1.3),Cm(1.3)
sec.left_margin,sec.right_margin=Cm(1.5),Cm(1.5)
sec.footer_distance=Cm(.6)
doc.add_heading('9 ISP Tool 原始截圖',1)
cap=doc.add_paragraph('圖 1　2DNR 參數頁面　對照本文的畫面位置與操作步驟')
cap.paragraph_format.space_after=Pt(5)
for run in cap.runs: run.font.size=Pt(10)
para=doc.add_paragraph()
para.alignment=WD_ALIGN_PARAGRAPH.CENTER
para.paragraph_format.space_after=Pt(3)
para.paragraph_format.line_spacing=1.0
shape=para.add_run().add_picture(str(root/'tmp/2dnr_sop_render/user_2dnr_screenshot.png'),width=Cm(26.1))
shape._inline.docPr.set('descr','使用者提供的 ISP Tool 2DNR 原始截圖，包含 Outlier、G Balance、Filter A、Filter B、Filter Control、LCA 與 DBCS。')
note=doc.add_paragraph('位置對照：左側為 Outlier／G Balance；中央為 Filter A／Filter Control／DBCS；右側為 Filter B／LCA。')
note.paragraph_format.space_after=Pt(0)
for run in note.runs: run.font.size=Pt(9)
doc.save(dst)
print(dst)
