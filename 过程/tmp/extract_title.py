import re, pdfplumber, os
PDF = r"C:\Users\jzc\Desktop\v3-纪泽操毕业论文.pdf"
title = None
with pdfplumber.open(PDF) as pdf:
    head_text = "\n".join((p.extract_text() or "") for p in pdf.pages[:10])
m = re.search(
    r"毕业论文题目[：:]\s*([\s\S]{1,150}?)(?=(?:工程硕士|专业学位|硕士生姓名|指导教师|学\s*校\s*代\s*码|软件工程专业|计算机.{0,4}专业|.{0,8}\d{4}\s*级\s*硕士生|本科生|学\s*号|姓\s*名))",
    head_text,
)
if m:
    title = re.sub(r"\s+", "", m.group(1)).strip()
if not title:
    m = re.search(
        r"论\s*文\s*题\s*目\s*([\s\S]{0,200}?)(?=作\s*者\s*姓\s*名|专业学位类别|研\s*究\s*方\s*向|学\s*号|姓\s*名|指导教师)",
        head_text,
    )
    if m:
        title = re.sub(r"\s+", "", m.group(1)).strip()
if not title or len(title) < 5:
    title = os.path.splitext(os.path.basename(PDF))[0]
    print(f"警告：未能从 PDF 内容抽取题目，回退到文件名: {title}")
title_clean = re.sub(r'[\\/:*?"<>|\r\n\t]', "", title).strip()
if len(title_clean) > 120:
    title_clean = title_clean[:120]
print(f"PAPER_TITLE={title_clean}")
print("---HEAD---")
print(head_text[:4000])
