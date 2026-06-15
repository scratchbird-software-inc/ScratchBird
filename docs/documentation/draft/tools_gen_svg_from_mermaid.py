#!/usr/bin/env python3
# Generate simple, Inkscape-friendly SVGs from a (subset of) mermaid source.
# Every shape carries explicit stroke/fill; all labels are real <text>; no CSS, no foreignObject.
import re, sys, html, os

FONT = "Helvetica, Arial, sans-serif"
FS = 13           # node font size
EFS = 11          # edge/label font size
CW = 7.2          # approx char width at FS
LH = 17           # line height
PADX, PADY = 16, 10
NODE_FILL = "#eef3fb"; NODE_STROKE = "#3b5b92"; NODE_TEXT = "#15233f"
EDGE = "#555555"; LABEL_TEXT = "#333333"; LABEL_BG = "#ffffff"
SUB_STROKE = "#9aa7bd"; START_FILL = "#333333"
MARGIN = 24

def esc(s): return html.escape(s, quote=True)

def clean_label(raw):
    s = raw.strip().strip('"')
    s = re.sub(r'<br\s*/?>', '\n', s, flags=re.I)
    s = re.sub(r'</?b>|</?i>|</?strong>|</?em>', '', s, flags=re.I)
    s = s.replace('&quot;','"').replace('&amp;','&')
    return s

def text_lines(label): return [ln for ln in label.split('\n')] or [""]
def node_w(label):
    return max(70, int(max((len(l) for l in text_lines(label)), default=0)*CW)+2*PADX)
def node_h(label):
    return max(34, len(text_lines(label))*LH + 2*PADY - 4)

def svg_text(cx, cy, lines, fs=FS, color=NODE_TEXT, anchor="middle", weight="normal"):
    n=len(lines); total=(n-1)*LH
    y0=cy - total/2
    out=[f'<text x="{cx:.1f}" y="{y0:.1f}" font-family="{FONT}" font-size="{fs}" fill="{color}" '
         f'text-anchor="{anchor}" dominant-baseline="middle" font-weight="{weight}">']
    for i,ln in enumerate(lines):
        dy = 0 if i==0 else LH
        out.append(f'<tspan x="{cx:.1f}" dy="{dy if i else 0:.1f}">{esc(ln)}</tspan>')
    out.append('</text>')
    return ''.join(out)

def header(w,h):
    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{int(w)}" height="{int(h)}" '
            f'viewBox="0 0 {int(w)} {int(h)}" font-family="{FONT}">\n'
            f'<defs><marker id="arrow" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="8" markerHeight="8" '
            f'orient="auto-start-end"><path d="M0,0 L10,5 L0,10 z" fill="{EDGE}" stroke="{EDGE}"/></marker></defs>\n'
            f'<rect x="0" y="0" width="{int(w)}" height="{int(h)}" fill="#ffffff"/>\n')

def shape_svg(shape, x, y, w, h, label):
    cx, cy = x+w/2, y+h/2
    lines=text_lines(label)
    if shape in ('round','stadium'):
        rx = h/2 if shape=='stadium' else 8
        body=f'<rect x="{x:.1f}" y="{y:.1f}" width="{w:.1f}" height="{h:.1f}" rx="{rx:.1f}" ry="{rx:.1f}" fill="{NODE_FILL}" stroke="{NODE_STROKE}" stroke-width="1.5"/>'
    elif shape=='diamond':
        body=f'<polygon points="{cx:.1f},{y:.1f} {x+w:.1f},{cy:.1f} {cx:.1f},{y+h:.1f} {x:.1f},{cy:.1f}" fill="{NODE_FILL}" stroke="{NODE_STROKE}" stroke-width="1.5"/>'
    elif shape=='circle':
        r=max(w,h)/2
        body=f'<circle cx="{cx:.1f}" cy="{cy:.1f}" r="{r:.1f}" fill="{NODE_FILL}" stroke="{NODE_STROKE}" stroke-width="1.5"/>'
    elif shape=='subroutine':
        body=(f'<rect x="{x:.1f}" y="{y:.1f}" width="{w:.1f}" height="{h:.1f}" fill="{NODE_FILL}" stroke="{NODE_STROKE}" stroke-width="1.5"/>'
              f'<line x1="{x+6:.1f}" y1="{y:.1f}" x2="{x+6:.1f}" y2="{y+h:.1f}" stroke="{NODE_STROKE}" stroke-width="1.5"/>'
              f'<line x1="{x+w-6:.1f}" y1="{y:.1f}" x2="{x+w-6:.1f}" y2="{y+h:.1f}" stroke="{NODE_STROKE}" stroke-width="1.5"/>')
    else:
        body=f'<rect x="{x:.1f}" y="{y:.1f}" width="{w:.1f}" height="{h:.1f}" fill="{NODE_FILL}" stroke="{NODE_STROKE}" stroke-width="1.5"/>'
    return body + svg_text(cx, cy, lines)

# ---------- flowchart / state ----------
SHAPE_PATTERNS = [
    ('subroutine', re.compile(r'^\[\[(.*)\]\]$', re.S)),
    ('circle',     re.compile(r'^\(\((.*)\)\)$', re.S)),
    ('stadium',    re.compile(r'^\(\[(.*)\]\)$', re.S)),
    ('diamond',    re.compile(r'^\{\{?(.*?)\}?\}$', re.S)),
    ('round',      re.compile(r'^\((.*)\)$', re.S)),
    ('asym',       re.compile(r'^>(.*)\]$', re.S)),
    ('rect',       re.compile(r'^\[(.*)\]$', re.S)),
]
NODE_TOKEN = re.compile(r'([A-Za-z0-9_.]+)\s*(\[\[.*?\]\]|\(\(.*?\)\)|\(\[.*?\]\)|\{\{.*?\}\}|\{.*?\}|\(.*?\)|\[.*?\]|>.*?\])?')

def parse_node_token(tok):
    m=NODE_TOKEN.match(tok.strip())
    if not m: return None
    nid=m.group(1); rest=m.group(2)
    shape='rect'; label=None
    if rest:
        for sh,pat in SHAPE_PATTERNS:
            mm=pat.match(rest)
            if mm:
                shape={'asym':'rect','diamond':'diamond'}.get(sh,sh); label=clean_label(mm.group(1)); break
    return nid, shape, label

def normalize_inline_labels(line):
    line=re.sub(r'--\s*([^->|][^>|]*?)\s*-->', lambda m:'-->|'+m.group(1).strip()+'|', line)
    line=re.sub(r'==\s*([^=>|][^>|]*?)\s*==>', lambda m:'==>|'+m.group(1).strip()+'|', line)
    line=re.sub(r'-\.\s*([^.>|][^>|]*?)\s*\.->', lambda m:'-.->|'+m.group(1).strip()+'|', line)
    return line

EDGE_OP = re.compile(r'(-\.->|-\.-|--x|--o|-->|---|==>|===)\s*(?:\|([^|]*)\|)?')

def parse_flow(src, statelike=False):
    lines=src.splitlines()
    direction='TB'
    nodes={}; order=[]; edges=[]
    def reg(nid, shape='rect', label=None):
        if nid not in nodes:
            nodes[nid]={'shape':shape,'label':label if label is not None else nid}; order.append(nid)
        else:
            if label is not None: nodes[nid]['label']=label
            if shape!='rect': nodes[nid]['shape']=shape
    first=True
    for raw in lines:
        line=raw.split('%%')[0].strip()
        if not line: continue
        if first:
            m=re.match(r'(?:flowchart|graph|stateDiagram(?:-v2)?)\s+([A-Za-z]{2})?', line)
            if m and m.group(1): direction=m.group(1).upper()
            first=False
            if re.match(r'(?:flowchart|graph|stateDiagram)', line): continue
        low=line.lower()
        if low.startswith(('classdef','class ','style ','linkstyle','click','direction','subgraph','end','accTitle','note')):
            # state notes/composite handled loosely; skip these directives
            if statelike and low.startswith('state '):
                pass
            else:
                continue
        line=normalize_inline_labels(line)
        if statelike:
            line=line.replace('[*]','__START__')
        # split into refs by edge operators
        ops=list(EDGE_OP.finditer(line))
        if ops:
            segs=[]; idx=0
            for mo in ops:
                segs.append((line[idx:mo.start()], mo.group(2)))
                idx=mo.end()
            segs.append((line[idx:], None))
            ref_strs=[segs[0][0]]+[s[0] for s in segs[1:]]
            lbls=[s[1] for s in segs[:-1]]
            parsed=[]
            for rs in ref_strs:
                # handle a & b
                sub=[]
                for part in rs.split('&'):
                    part=part.strip().rstrip(':')
                    if not part: continue
                    p=parse_node_token(part)
                    if p: sub.append(p)
                parsed.append(sub)
            for grp in parsed:
                for nid,shape,label in grp:
                    if statelike and nid=='__START__':
                        reg(nid,'startend',''); continue
                    reg(nid,shape,label)
            for i in range(len(parsed)-1):
                lbl=lbls[i] if i < len(lbls) else None
                for a in parsed[i]:
                    for b in parsed[i+1]:
                        edges.append((a[0],b[0], clean_label(lbl) if lbl else ''))
        else:
            # state line like:  S : description   or bare node def
            if statelike and ' : ' in line:
                nid,desc=line.split(' : ',1); nid=nid.strip()
                if nid=='__START__': nid='__START__'
                reg(nid,'round' if nid!='__START__' else 'startend', clean_label(desc) if nid!='__START__' else '')
                continue
            p=parse_node_token(line)
            if p:
                nid,shape,label=p
                if statelike and nid=='__START__': reg(nid,'startend','')
                else: reg(nid, 'round' if statelike else shape, label)
    if statelike:
        for n in nodes:
            if n!='__START__' and nodes[n]['shape']=='rect': nodes[n]['shape']='round'
    return direction, nodes, order, edges

def layered_layout(direction, nodes, order, edges):
    succ={n:[] for n in nodes}; indeg={n:0 for n in nodes}
    for a,b,_ in edges:
        if a in nodes and b in nodes:
            succ[a].append(b); indeg[b]+=1
    # longest-path depth
    depth={n:0 for n in nodes}
    # process in topological-ish order via repeated relaxation
    for _ in range(len(nodes)+1):
        changed=False
        for a,b,_ in edges:
            if a in depth and b in depth and depth[b] < depth[a]+1:
                depth[b]=depth[a]+1; changed=True
        if not changed: break
    layers={}
    for n in order: layers.setdefault(depth[n],[]).append(n)
    horizontal = direction in ('LR','RL')
    # node sizes
    size={n:(node_w(nodes[n]['label']) if nodes[n]['shape']!='startend' else 18,
             node_h(nodes[n]['label']) if nodes[n]['shape']!='startend' else 18) for n in nodes}
    GAPX, GAPY = 60, 34
    pos={}
    maxdepth=max(layers) if layers else 0
    # cross-axis cursor per layer
    if horizontal:
        x=MARGIN
        # column widths
        for d in range(maxdepth+1):
            col=layers.get(d,[])
            colw=max([size[n][0] for n in col], default=0)
            y=MARGIN
            for n in col:
                w,h=size[n]; pos[n]=(x+(colw-w)/2, y, w, h); y+=h+GAPY
            x+=colw+GAPX
    else:
        y=MARGIN
        for d in range(maxdepth+1):
            row=layers.get(d,[])
            rowh=max([size[n][1] for n in row], default=0)
            x=MARGIN
            for n in row:
                w,h=size[n]; pos[n]=(x, y+(rowh-h)/2, w, h); x+=w+GAPX
            y+=rowh+GAPY
    W=max([pos[n][0]+pos[n][2] for n in pos], default=100)+MARGIN
    H=max([pos[n][1]+pos[n][3] for n in pos], default=100)+MARGIN
    return pos, W, H, horizontal

def anchor(p, side):
    x,y,w,h=p
    return {'r':(x+w,y+h/2),'l':(x,y+h/2),'t':(x+w/2,y),'b':(x+w/2,y+h/2+h/2)}.get(side,(x+w/2,y+h/2)) if side!='b' else (x+w/2,y+h)

def render_flow(src, statelike=False):
    direction,nodes,order,edges=parse_flow(src,statelike)
    if not nodes: return None
    pos,W,H,horizontal=layered_layout(direction,nodes,order,edges)
    out=[header(W,H)]
    # edges first
    for a,b,lbl in edges:
        if a not in pos or b not in pos: continue
        pa,pb=pos[a],pos[b]
        if horizontal:
            x1,y1=pa[0]+pa[2],pa[1]+pa[3]/2; x2,y2=pb[0],pb[1]+pb[3]/2
        else:
            x1,y1=pa[0]+pa[2]/2,pa[1]+pa[3]; x2,y2=pb[0]+pb[2]/2,pb[1]
        if pos[b][1]+ (0) and ( (horizontal and x2 < x1) or (not horizontal and y2 < y1) ):
            # backward edge: route straight anyway
            pass
        out.append(f'<line x1="{x1:.1f}" y1="{y1:.1f}" x2="{x2:.1f}" y2="{y2:.1f}" stroke="{EDGE}" stroke-width="1.4" marker-end="url(#arrow)"/>')
        if lbl:
            mx,my=(x1+x2)/2,(y1+y2)/2
            tw=len(lbl)*EFS*0.6+8
            out.append(f'<rect x="{mx-tw/2:.1f}" y="{my-9:.1f}" width="{tw:.1f}" height="16" fill="{LABEL_BG}" stroke="none"/>')
            out.append(f'<text x="{mx:.1f}" y="{my:.1f}" font-family="{FONT}" font-size="{EFS}" fill="{LABEL_TEXT}" text-anchor="middle" dominant-baseline="middle">{esc(lbl)}</text>')
    # nodes
    for n in order:
        if n not in pos: continue
        x,y,w,h=pos[n]; nd=nodes[n]
        if nd['shape']=='startend':
            cx,cy=x+w/2,y+h/2
            out.append(f'<circle cx="{cx:.1f}" cy="{cy:.1f}" r="8" fill="{START_FILL}" stroke="{START_FILL}"/>')
        else:
            out.append(shape_svg(nd['shape'], x, y, w, h, nd['label']))
    out.append('</svg>\n')
    return ''.join(out)

# ---------- sequence ----------
def render_sequence(src):
    lines=src.splitlines()
    participants=[]; labels={}; msgs=[]
    def add_p(pid,label=None):
        if pid not in participants: participants.append(pid); labels[pid]=label or pid
        elif label: labels[pid]=label
    for raw in lines:
        line=raw.split('%%')[0].strip()
        if not line or line.startswith('sequenceDiagram'): continue
        m=re.match(r'(?:participant|actor)\s+(\S+)(?:\s+as\s+(.+))?$', line)
        if m: add_p(m.group(1), clean_label(m.group(2)) if m.group(2) else None); continue
        if re.match(r'(?:loop|alt|opt|par|else|end|activate|deactivate|rect|critical|break|autonumber)\b', line):
            continue
        mn=re.match(r'[Nn]ote\s+(?:over|left of|right of)\s+([^:]+):\s*(.+)$', line)
        if mn:
            who=[w.strip() for w in mn.group(1).split(',')]
            for w in who: add_p(w)
            msgs.append(('note', who, clean_label(mn.group(2)))); continue
        mm=re.match(r'(\S+?)\s*(-{1,2}>>?|-{1,2}\)|-{1,2}x)\s*(\S+?)\s*:\s*(.+)$', line)
        if mm:
            a,op,b,txt=mm.groups(); add_p(a); add_p(b)
            dashed='--' in op
            msgs.append(('msg',a,b,clean_label(txt),dashed)); continue
    if not participants: return None
    # layout
    pw={p:max(80,len(labels[p])*CW+2*PADX) for p in participants}
    GAP=40; x=MARGIN; px={}
    for p in participants:
        px[p]=x+pw[p]/2; x+=pw[p]+GAP
    W=x-GAP+MARGIN
    top=MARGIN; boxh=34; head_bottom=top+boxh
    y=head_bottom+30; rowh=38
    body=[]
    for msg in msgs:
        if msg[0]=='note':
            who=msg[1]; txt=msg[2]
            xs=[px[w] for w in who if w in px]
            x1=min(xs)-50; x2=max(xs)+50; w=x2-x1
            body.append(f'<rect x="{x1:.1f}" y="{y-12:.1f}" width="{w:.1f}" height="26" fill="#fff8e1" stroke="#c9a227" stroke-width="1"/>')
            body.append(f'<text x="{(x1+x2)/2:.1f}" y="{y+1:.1f}" font-family="{FONT}" font-size="{EFS}" fill="{LABEL_TEXT}" text-anchor="middle" dominant-baseline="middle">{esc(txt)}</text>')
            y+=rowh; continue
        _,a,b,txt,dashed=msg
        if a not in px or b not in px: continue
        x1,x2=px[a],px[b]
        dash=' stroke-dasharray="5,4"' if dashed else ''
        if a==b:
            body.append(f'<path d="M{x1:.1f},{y:.1f} h40 v18 h-40" fill="none" stroke="{EDGE}" stroke-width="1.3"{dash} marker-end="url(#arrow)"/>')
            body.append(f'<text x="{x1+46:.1f}" y="{y-4:.1f}" font-family="{FONT}" font-size="{EFS}" fill="{LABEL_TEXT}" dominant-baseline="middle">{esc(txt)}</text>')
            y+=rowh+8
        else:
            body.append(f'<line x1="{x1:.1f}" y1="{y:.1f}" x2="{x2:.1f}" y2="{y:.1f}" stroke="{EDGE}" stroke-width="1.3"{dash} marker-end="url(#arrow)"/>')
            mx=(x1+x2)/2
            body.append(f'<text x="{mx:.1f}" y="{y-6:.1f}" font-family="{FONT}" font-size="{EFS}" fill="{LABEL_TEXT}" text-anchor="middle">{esc(txt)}</text>')
            y+=rowh
    H=y+10
    out=[header(W,H)]
    # lifelines
    for p in participants:
        out.append(f'<line x1="{px[p]:.1f}" y1="{head_bottom:.1f}" x2="{px[p]:.1f}" y2="{H-MARGIN:.1f}" stroke="#aaaaaa" stroke-width="1" stroke-dasharray="4,4"/>')
    # heads
    for p in participants:
        w=pw[p]; x0=px[p]-w/2
        out.append(f'<rect x="{x0:.1f}" y="{top:.1f}" width="{w:.1f}" height="{boxh}" rx="6" fill="{NODE_FILL}" stroke="{NODE_STROKE}" stroke-width="1.5"/>')
        out.append(svg_text(px[p], top+boxh/2, [labels[p]]))
    out.extend(body); out.append('</svg>\n')
    return ''.join(out)

def generate(src):
    head=src.strip().splitlines()[0].strip().lower() if src.strip() else ''
    if head.startswith('sequencediagram'): return render_sequence(src)
    if head.startswith('statediagram'): return render_flow(src, statelike=True)
    return render_flow(src, statelike=False)

def main():
    man=[l.rstrip('\n').split('\t') for l in open('/tmp/mmd/manifest.tsv') if l.strip()]
    ok=fail=0; fails=[]
    for mmd,out in man:
        try:
            src=open(mmd,encoding='utf-8',errors='replace').read()
            svg=generate(src)
            if not svg: raise ValueError('empty render')
            open(out,'w',encoding='utf-8').write(svg); ok+=1
        except Exception as e:
            fail+=1; fails.append((out,str(e)))
    print(f'generated {ok}, failed {fail}')
    for f in fails: print('  FAIL',f)

if __name__=='__main__': main()
