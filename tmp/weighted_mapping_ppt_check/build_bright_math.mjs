import fs from 'node:fs/promises';
import {pathToFileURL} from 'node:url';
import {Presentation,PresentationFile} from 'file:///C:/Users/pega_user/.cache/codex-runtimes/codex-primary-runtime/dependencies/node/node_modules/@oai/artifact-tool/dist/artifact_tool.mjs';
const skill='C:/Users/pega_user/.codex/plugins/cache/openai-primary-runtime/presentations/26.1104.22356/skills/presentations';
const work='D:/01_Project/02_Loewe/01_Report/20261110/WeightedMapping';
const build=work+'/.build';
await fs.mkdir(build,{recursive:true}); await fs.mkdir(work+'/output',{recursive:true});
const deck=Presentation.create({slideSize:{width:1280,height:720}});
const font='Microsoft JhengHei';
const ink='#162F3B',muted='#526570',accent='#007D7B';
function text(s,str,x,y,w,h,size=29,color=ink,bold=false){
 const sh=s.shapes.add({geometry:'textbox',position:{left:x,top:y,width:w,height:h},fill:'none',line:{fill:'none',width:0}});
 sh.text=str; sh.text.style={typeface:font,fontSize:size,color,bold};return sh;
}
function slide(title,n,notes){const s=deck.slides.add();s.background.fill='#FFFFFF';text(s,title,70,48,1140,88,44,ink,true);text(s,String(n).padStart(2,'0'),1160,662,55,30,18,muted);s.speakerNotes.text=notes;return s;}
function table(s,values,y,h,widths){
 const t=s.tables.add({rows:values.length,columns:values[0].length,left:70,top:y,width:1140,height:h,values,columnWidths:widths});
 t.borders.assign({fill:'#D6E0E3',width:1,style:'solid'});
 for(let r=0;r<values.length;r++)for(let c=0;c<values[0].length;c++){
  const cell=t.getCell(r,c);cell.fill=r===0?'#E8F3F2':'#FFFFFF';
  cell.text.style={typeface:font,fontSize:27,color:ink,bold:r===0};
 }
 return t;
}
let s=slide('Weighted Mapping：P80 門檻計算修正',1,
 '來源：本次對話中已確認的舊流程，以及目前 Enhancement/AGC/agc_weighted.c:312–354。現行程式先複製input histogram到scratch，執行Clip與選用Redistribution，再以scratch的processed_pixels總數及order_dn求Bright Percentile門檻。此簡報專門說明百分位門檻使用的直方圖，不涉及RAW14量化、Gain Limit或FFC。');
text(s,'原本：P80 用輸入直方圖算，卻套在處理後的直方圖',70,157,1140,85,35,ink,true);
text(s,'Clip／Redistribution 已改變各灰階的分布比例，原門檻不再代表新的 P80',70,270,1140,90,29,muted);
text(s,'修正後的順序',70,395,1140,50,32,accent,true);
text(s,'輸入直方圖 → Clip → Redistribution（若開啟）',70,465,1140,55,31);
text(s,'→ 用處理後直方圖重算 P80 → Bright Weight → Mapping',70,538,1140,65,31,accent,true);
text(s,'P80 是例子；其他 Bright Percentile 設定也採用相同原則',70,635,1140,35,23,muted);
s=slide('數字範例：Clip 後的 P80 已改變',2,
 '可重現示意，不是實拍資料。輸入五個DN的計數[70,10,10,5,5]，共100；Clip上限10，不做Redistribution，處理後[10,10,10,5,5]，共40。依目前程式的線性插值百分位：rank=(N-1)*0.8。原始rank79.2，zero-based排序第79筆200、第80筆300，得220；處理後rank31.2，第31/32筆皆400，得400。若套用Clip Limit參數，pixels100/unique5*0.5=10，也就是Clip Limit0.5。');
text(s,'假設 P80、每個灰階最多保留 10 筆，Redistribution 關閉',70,146,1140,65,29);
table(s,[['灰階 DN','輸入直方圖：筆數','Clip 後：筆數'],['100','70','10'],['200','10','10'],['300','10','10'],['400','5','5'],['500','5','5']],234,285,[300,420,420]);
text(s,'原本用輸入 100 筆算：P80 = 220 DN',70,547,1140,50,29);
text(s,'修正用保留的 40 筆算：P80 = 400 DN',70,606,1140,55,31,accent,true);
const {applyPresentationChartFont}=await import(pathToFileURL(skill+'/container_tools/artifact_tool_utils.mjs').href);
function histogram(s,x,heading,counts,color){
 text(s,heading,x,155,540,55,29,color,true);
 const c=s.charts.add('bar',{
  position:{left:x,top:225,width:535,height:335},
  categories:['100','200','300','400','500'],
  series:[{name:'筆數',values:counts,fill:color}],
  hasLegend:false,barOptions:{direction:'column',grouping:'clustered',gapWidth:70},
  xAxis:{title:'灰階 DN',textStyle:{fontSize:22,fill:ink}},
  yAxis:{min:0,max:80,majorUnit:20,title:'筆數',textStyle:{fontSize:21,fill:muted},majorGridlines:{fill:'#D6E0E3',width:1}},
  dataLabels:{showValue:true,position:'outEnd',textStyle:{fontSize:24,fill:ink,bold:true}}
 });applyPresentationChartFont(c,{fontFamily:font});
}
s=slide('圖例一：Clip 改變分布，P80 也必須重算',3,
 '示意資料與上一頁相同：DN100/200/300/400/500，input counts70/10/10/5/5，Clip cap10，Redistribution off；處理後counts10/10/10/5/5。左右Y軸均為0–80以避免視覺比例誤導。P80採線性插值，220->400 DN；應隨保留筆數重算。圖表均為可編輯原生物件。');
histogram(s,70,'輸入：100 筆，P80 = 220 DN',[70,10,10,5,5],'#C97936');
histogram(s,670,'Clip 後：40 筆，P80 = 400 DN',[10,10,10,5,5],accent);
text(s,'100 DN 的高峰由 70 筆變成 10 筆；亮暗分布比例已改變',70,583,1140,55,29);
text(s,'門檻應跟著新分布移動，不能沿用輸入的 220 DN',70,639,1140,37,24,accent,true);
s=slide('圖例二：舊門檻會擴大 Bright Weight 範圍',4,
 'Bright Weight=3示意，非實拍。按現行interval_weight_float：以每1DN區間中點i+0.5和threshold比較。本圖選取i=100/200/300/400，這些區間的處理後base h[i]+h[i+1]均非零。舊threshold220對中點100.5/200.5/300.5/400.5的倍率為1/1/3/3；新threshold400為1/1/1/3。圖顯示額外Bright Weight倍率，沒有顯示mapping output或雜訊。');
text(s,'假設 Bright Weight = 3；比較同一份 Clip 後直方圖',70,148,1140,55,29);
const c=s.charts.add('bar',{
 position:{left:80,top:240,width:1100,height:310},categories:['100.5','200.5','300.5','400.5'],
 series:[{name:'沿用舊 P80 = 220',values:[1,1,3,3],fill:'#C97936'},{name:'重算後 P80 = 400',values:[1,1,1,3],fill:accent}],
 hasLegend:true,legend:{position:'top',textStyle:{fontSize:23,fill:ink}},
 barOptions:{direction:'column',grouping:'clustered',gapWidth:130},
 xAxis:{title:'選取的 1 DN 區間中點',textStyle:{fontSize:23,fill:ink}},
 yAxis:{min:0,max:4,majorUnit:1,title:'加權倍率',textStyle:{fontSize:21,fill:muted},majorGridlines:{fill:'#D6E0E3',width:1}},
 dataLabels:{showValue:true,position:'outEnd',textStyle:{fontSize:24,fill:ink,bold:true}}
});applyPresentationChartFont(c,{fontFamily:font});
text(s,'300.5 DN：原本被乘以 3；重算門檻後，只保留原權重',70,585,1140,55,29,accent,true);
text(s,'這裡比較的是加權倍率，不是輸出灰階或降噪量',70,643,1140,35,23,muted);
s=slide('Bright Weight 怎麼算：先加權，再累加映射',5,
 '來源：Enhancement/AGC/agc_weighted.c:73–79、364–381。為了用少量項目完整算出Mapping，本頁另用五個連續DN100/101/102/103/104，處理後計數10/10/10/5/5；不同於前頁相隔100DN的示例。N40，P80 rank31.2，對應DN103。每1DN區間基礎權重H(i)+H(i+1)，中點i+0.5>=103者乘Bright Weight3。四區間權重20/20/15/30，總和85。LUT最低DN100為0；LUT103=round((20+20+15)/85*16383)=10601。純Weighted LUT示例，不含後續Gain Limit、Smooth、Background、Large Scale；不是對原始像素直接乘3。');
text(s,'另用連續 DN 小例子：P80 = 103 DN，Bright Weight = 3',70,145,1140,55,29);
text(s,'區間權重 = [ H(i) + H(i+1) ] × 倍率；亮部 ×3，其餘 ×1',70,207,1140,55,27,accent,true);
table(s,[['DN 區間（中點）','處理後筆數相加','倍率','加權後權重'],['100～101（100.5）','10 + 10 = 20','×1','20'],['101～102（101.5）','10 + 10 = 20','×1','20'],['102～103（102.5）','10 + 5 = 15','×1','15'],['103～104（103.5）','5 + 5 = 10','×3','30']],277,240,[350,350,180,260]);
text(s,'H 是 Clip／Redistribution 後的筆數；區間中點 ≥ P80 才乘以 3',70,533,1140,45,24,muted);
text(s,'映射到輸入 103：累加權重 55 ÷ 總權重 85 × 16383',70,585,1140,45,27);
text(s,'四捨五入得到 10,601 DN；不是把原始像素直接乘以 3',70,636,1140,40,25,accent,true);
s=slide('修正結果：門檻與加權使用同一份分布',6,
 '目前 Enhancement/AGC/agc_weighted.c:345–354，processed_pixels=sum(scratch)，rank=(processed_pixels-1)*bright_percentile_tenths，order_dn(scratch,... )，其後用同一scratch做interval_weight。門檻與權重在每張影格以處理後分布重算；分塊實作應各自依tile處理後分布重算。本例interval midpoints250/350的舊threshold220會觸發Bright Weight，新threshold400不觸發；midpoint450則兩者都觸發。P值未改，DN門檻改變。修正保證計算來源與流程一致，不宣稱所有閃爍或感測器雜訊因此消除。');
text(s,'P80 的設定不變；改的是計算門檻所使用的資料',70,151,1140,70,33,accent,true);
table(s,[['比較項目','原本','修正後'],['P80 的資料來源','輸入直方圖','Clip／Redistribution 後'],['範例 P80 門檻','220 DN','400 DN'],['300.5 DN 區間中點','會被 Bright Weight 加權','不會被 Bright Weight 加權']],260,284,[390,350,400]);
text(s,'避免用舊門檻錯分亮部範圍；加權與後續映射採用一致的分布',70,577,1140,65,28);
text(s,'這是百分位計算順序的修正；不等於降噪，也不保證所有亮度跳動都消失',70,640,1140,40,22,muted);
for(let i=0;i<deck.slides.items.length;i++){
 const blob=await deck.export({slide:deck.slides.items[i],format:'png',scale:1});
 await fs.writeFile(`${build}/bright-math-slide-${i+1}.png`,new Uint8Array(await blob.arrayBuffer()));
}
const draft=build+'/percentile_candidate_bright_weight.pptx';await(await PresentationFile.exportPptx(deck)).save(draft);
const {finalizePresentation}=await import(pathToFileURL(skill+'/container_tools/artifact_tool_utils.mjs').href);
const result=await finalizePresentation({workspaceDir:work,candidatePath:draft,finalPath:work+'/output/WeightedMapping_Percentile修正說明_bright_weight.pptx',pythonExecutable:'C:/Users/pega_user/AppData/Local/Programs/Python/Python313/python.exe',integrityValidatorPath:skill+'/container_tools/inspect_presentation_package_integrity.py',layoutValidatorPath:skill+'/container_tools/inspect_presentation_layout_geometry.py',layoutArgs:['--expected-slide-size-emu','12192000,6858000','--validate-heading-fit','--require-native-table-slide','2','--require-native-table-slide','5','--require-native-table-slide','6'],explicitTotalSlideCount:6,requiredNativeTableOwnerSlides:[2,5,6],requiredNativeChartOwnerSlides:[3,4],materializeLiteralChartWorkbooks:true,fontPolicy:{basis:'design',families:[font]},verifyArtifactToolImport:true,receiptPath:build+'/bright_weight_validation.json'});
console.log(result.finalPath);
