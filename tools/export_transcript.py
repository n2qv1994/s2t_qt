#!/usr/bin/env python3
"""Lấy bản chép ĐẦY ĐỦ của một cuộc họp ra khỏi hệ thống, và xuất .docx.

Vì sao cần công cụ này, thay vì đọc khung "Phụ đề" trên màn hình: khung đọc chỉ
giữ **15 phút gần nhất**. Đó là quyết định có chủ ý trong client —

    // s2t-qt-client/core/TranscriptModel.cpp
    const double kLiveRowCacheSec     = 15 * 60;
    const int    kLiveRowCacheMaxRows = 180;

— để một cuộc họp dài không làm client phình vô hạn. Hệ quả: chụp màn hình
khung đọc lúc cuộc họp kết thúc thì chỉ được đoạn cuối, và **không có gì báo
cho biết là đang thiếu**. Đo được ngày 2026-09-02 trên một cuộc họp 88 phút:
ảnh chụp phút 40 chứa `[24:57]`–`[39:41]`, mất sạch 25 phút đầu.

Toàn văn thì vẫn luôn nằm đủ trên server (`LiveTranscript` giữ hết, và
`SessionStore` lưu vào SQLite khi dừng). Đường lấy nó là `get_review_state` —
tức nút **Soát & sửa (F9)** trên giao diện — vốn không dùng bộ đệm ấy.

Cách chạy (cần python3 + stub sinh từ docs/danh-sach-api.md; xem
tools/interop_check.py):

    python3 tools/export_transcript.py --target 127.0.0.1:8800 --token s2t-local \\
                                       --session <session_id> --out ban-chep

Không truyền --session thì nó liệt kê các phiên đang có rồi thoát.

Tệp .docx được dựng bằng thư viện chuẩn, không cần python-docx: .docx vốn là
một tệp ZIP chứa XML. Máy RHEL không có python-docx và cũng không nên phải cài
thêm chỉ để xuất một bản chép.
"""

import argparse
import sys
import zipfile
from xml.sax.saxutils import escape

import grpc

import asr_session_pb2 as pb
import asr_session_pb2_grpc as pb_grpc


def fetch_rows(stub, md, session):
    """Toàn bộ dòng của một phiên, theo thứ tự thời gian."""
    request = pb.ReviewRequest(session_id=session)
    state = stub.get_review_state(request, metadata=md).state
    return sorted(state.rows, key=lambda r: r.start_sec)


def group_by_speaker(rows):
    """Gộp các dòng liên tiếp của cùng một người thành một đoạn.

    Pipeline cắt dòng theo từng lượt phân vai, nên sinh ra rất nhiều dòng một
    chữ ("dạ", "ờ", "vâng"). Hợp lý cho dải thời gian, không đọc được thành văn
    bản. Mốc thời gian giữ của dòng đầu tiên trong đoạn.
    """
    blocks = []
    for row in rows:
        text = row.merged_text.strip()
        if not text:
            continue
        who = row.verified_name.strip() or f"Người {row.speaker}"
        if blocks and blocks[-1][1] == who:
            blocks[-1][2] += " " + text
        else:
            blocks.append([row.start_sec, who, text])
    return blocks


def stamp(seconds):
    minutes, secs = divmod(int(seconds), 60)
    return f"{minutes:02d}:{secs:02d}"


# ------------------------------------------------------------------ .docx

W = 'xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main"'


def _run(text, bold=False, size=22, color=None):
    rpr = "<w:rPr>"
    if bold:
        rpr += "<w:b/>"
    if color:
        rpr += f'<w:color w:val="{color}"/>'
    rpr += f'<w:sz w:val="{size}"/><w:szCs w:val="{size}"/></w:rPr>'
    return f'<w:r>{rpr}<w:t xml:space="preserve">{escape(text)}</w:t></w:r>'


def _para(runs, after=120):
    return f'<w:p><w:pPr><w:spacing w:after="{after}"/></w:pPr>{"".join(runs)}</w:p>'


def write_docx(path, title, blocks):
    words = {}
    for _, who, text in blocks:
        words[who] = words.get(who, 0) + len(text.split())
    total = sum(words.values())

    body = [_para([_run(title, bold=True, size=32)], after=200)]
    body.append(_para([_run(f"Tổng: {len(blocks)} đoạn, {total} chữ, "
                            f"{len(words)} người nói.", size=20, color="666666")]))
    for who, count in sorted(words.items(), key=lambda kv: -kv[1]):
        share = 100.0 * count / total if total else 0.0
        body.append(_para([_run(f"    {who}: {count} chữ ({share:.1f}%)",
                                size=20, color="666666")], after=60))
    body.append(_para([_run("", size=20)], after=200))
    for start, who, text in blocks:
        body.append(_para([_run(f"[{stamp(start)}] ", size=18, color="888888"),
                           _run(f"{who}: ", bold=True),
                           _run(text)]))

    head = '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
    document = (head + f"<w:document {W}><w:body>{''.join(body)}"
                '<w:sectPr><w:pgSz w:w="11906" w:h="16838"/>'
                '<w:pgMar w:top="1134" w:right="1134" w:bottom="1134" w:left="1134"/>'
                "</w:sectPr></w:body></w:document>")
    styles = (head + f"<w:styles {W}><w:docDefaults><w:rPrDefault><w:rPr>"
              '<w:rFonts w:ascii="Calibri" w:hAnsi="Calibri" w:cs="Calibri"/>'
              '<w:sz w:val="22"/></w:rPr></w:rPrDefault></w:docDefaults></w:styles>')
    content_types = (head +
        '<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">'
        '<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>'
        '<Default Extension="xml" ContentType="application/xml"/>'
        '<Override PartName="/word/document.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml"/>'
        '<Override PartName="/word/styles.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.styles+xml"/>'
        "</Types>")
    rels = (head +
        '<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
        '<Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="word/document.xml"/>'
        "</Relationships>")
    doc_rels = (head +
        '<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
        '<Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles" Target="styles.xml"/>'
        "</Relationships>")

    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("[Content_Types].xml", content_types)
        z.writestr("_rels/.rels", rels)
        z.writestr("word/_rels/document.xml.rels", doc_rels)
        z.writestr("word/document.xml", document)
        z.writestr("word/styles.xml", styles)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--target", default="127.0.0.1:8800")
    parser.add_argument("--token", default="")
    parser.add_argument("--session", default="")
    parser.add_argument("--out", default="ban-chep")
    parser.add_argument("--title", default="")
    args = parser.parse_args()

    md = [("authorization", "Bearer " + args.token)] if args.token else []
    stub = pb_grpc.ProductASRServiceStub(grpc.insecure_channel(args.target))

    if not args.session:
        listed = stub.list_sessions(pb.ListSessionsRequest(), metadata=md)
        print("Chưa chọn phiên. Các phiên đang có:")
        for item in listed.sessions:
            print(f"  {item.session_id}  {item.title}")
        return 1

    rows = fetch_rows(stub, md, args.session)
    if not rows:
        print(f"phiên {args.session} không có dòng nào", file=sys.stderr)
        return 2
    blocks = group_by_speaker(rows)

    with open(args.out + ".txt", "w", encoding="utf-8") as handle:
        for start, who, text in blocks:
            handle.write(f"[{stamp(start)}] {who}: {text}\n")
    write_docx(args.out + ".docx", args.title or f"Bản chép {args.session}", blocks)

    print(f"{len(rows)} dòng -> {len(blocks)} đoạn")
    print(f"  {args.out}.txt")
    print(f"  {args.out}.docx")
    return 0


if __name__ == "__main__":
    sys.exit(main())
