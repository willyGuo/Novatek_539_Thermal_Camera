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
let s=slide('Weighted Mapping 修正說明',1,
 '來源：目前工作目錄 Enhancement/AGC/agc_weighted.c，對照 git HEAD 42497e3 的同檔。此簡報的「原本」指修正前的實作，不代表所有歷史版本。主機數值驗證不等於板端完整 ISP 或 UVC 品質驗證。');
text(s,'保留 RAW14 精度，改善亮度補償',70,188,1140,82,51,accent,true);
text(s,'原本：映射先量化成 256 階，再放大回 RAW14',70,328,1130,60,31);
text(s,'修正：直接建立 RAW14 映射，重新計算 Gain Limit 補償',70,412,1130,75,31);
text(s,'另以 Bright Weight = 1 改善已觀察到的整張亮度跳動',70,525,1130,60,27,muted);
text(s,'Tiffany_thermal  ·  2026/11/10',70,650,950,30,20,muted);
s=slide('原本的精度問題與修正',2,
 '原本程式：HEAD:Enhancement/AGC/agc_weighted.c，out[x]=(ws->lut[dn]*16383U+127U)/255U；LUT 先量化至 Gray8。現行程式第355–381行以16383建立LUT，輸出直接使用RAW14映射。表格為量化機制示意，假設理想映射目標1000/1001/1002，先 round(target*255/16383) 均為16，再 round(16*16383/255)=1028；不是實拍像素或對實際畫質的承諾。');
text(s,'先縮成 256 階，之後放大也補不回中間的細節',70,145,1140,58,30);
table(s,[['映射後目標（示意）','原本：先量化再放大','修正：直接 RAW14'],['1,000 DN','1,028 DN','1,000 DN'],['1,001 DN','1,028 DN','1,001 DN'],['1,002 DN','1,028 DN','1,002 DN']],236,280,[350,420,370]);
text(s,'原本量化階距約 64 DN；修正後可表達 1 DN 的差異',70,553,1140,58,30,accent,true);
text(s,'此表為原理示意；最終顯示仍會轉成 8-bit',70,625,1140,35,23,muted);
s=slide('Gain Limit 的亮度補償修正',3,
 '現行 Enhancement/AGC/agc_weighted.c:401–432。Gain Limit限制LUT斜率；Gray/RAW DN單位換算到RAW14，比例16383/255。修正前以lower-median DN作亮度錨點；現行以原始histogram對限制前後LUT差值加權平均，並按Smooth設定平滑offset。若裁切到0/16383，不保證最終均值完全相同；亦不保證不同影格的平均亮度鎖定。');
text(s,'原本：用單一中位數位置決定整張的亮度補償',70,163,1140,65,31);
text(s,'低對比場景中，中位數只移動 1 DN，也可能跨過很大的映射落差',70,245,1140,90,28,muted);
text(s,'修正：依全部像素，計算限制前後的平均亮度差',70,374,1140,65,31,accent,true);
text(s,'再把這個差值補回，減少單一中位數跳動帶來的突然變亮／變暗',70,456,1140,90,28);
text(s,'這項補償不會固定每張影像的亮度；Bright Weight 仍可能影響穩定性',70,603,1140,55,24,muted);
s=slide('驗證結果與目前改善範圍',4,
 '主機驗證：tests/test_weighted_raw14.py，本次執行116組獨立Weighted curves與完整均勻RAW14 ramp，identity輸出16384階；另驗證Gain Limit單位、mean補償、Q8 smoothing、49:51/51:49低對比回歸12組組合。報告workspace/tmp/weighted_mapping_ppt_check/weighted_raw14_report.json。亮度數據來源：D:/01_Project/02_Loewe/01_Report/20261109/codex/3/recording_63113_0000/analysis/agc_actual_params_replay.json。每組12張corrected RAW重複10輪，以當時實際Weighted參數回播，取first12_gray_pp；BW3與BW1其餘參數相同，Gain Limit2、Smooth0、P80、Clip0、Redistribution0、Background0、LargeScale0。Gray為RAW14*255/16383的等效8-bit單位；全畫面平均亮度的最大值減最小值，不是像素雜訊SD。使用者曾回報Bright Weight改1後不跳；此為參數調整，非BW3演算法已修正。');
text(s,'精度驗證：均勻 RAW14 漸層可保留 16,384 階',70,147,1140,56,31,accent,true);
text(s,'116 組映射曲線及 Gain Limit 回歸測試通過（主機）',70,214,1140,50,26);
table(s,[['12 張 RAW 重播','Bright Weight = 3','Bright Weight = 1'],['場景組：平均亮度變動','4.570 Gray','0.014 Gray'],['均溫黑體：平均亮度變動','2.689 Gray','0.014 Gray']],309,213,[480,330,330]);
text(s,'Bright Weight 改為 1 後，使用者回報整張亮度不再跳動',70,548,1140,55,27);
text(s,'亮度變動＝全畫面平均值的最大差；不是降噪量。4 DN 與上游雜訊仍須另外查證',70,622,1140,55,23,muted);
for(let i=0;i<deck.slides.items.length;i++){
 const blob=await deck.export({slide:deck.slides.items[i],format:'png',scale:1});
 await fs.writeFile(`${build}/slide-${i+1}.png`,new Uint8Array(await blob.arrayBuffer()));
}
const draft=build+'/candidate.pptx';await(await PresentationFile.exportPptx(deck)).save(draft);
console.log('DRAFT',draft);
const {finalizePresentation}=await import(pathToFileURL(skill+'/container_tools/artifact_tool_utils.mjs').href);
const result=await finalizePresentation({workspaceDir:work,candidatePath:draft,finalPath:work+'/output/WeightedMapping_修正說明.pptx',pythonExecutable:'C:/Users/pega_user/AppData/Local/Programs/Python/Python313/python.exe',integrityValidatorPath:skill+'/container_tools/inspect_presentation_package_integrity.py',layoutValidatorPath:skill+'/container_tools/inspect_presentation_layout_geometry.py',layoutArgs:['--expected-slide-size-emu','12192000,6858000','--validate-heading-fit','--require-native-table-slide','2','--require-native-table-slide','4'],explicitTotalSlideCount:4,requiredNativeTableOwnerSlides:[2,4],requiredNativeChartOwnerSlides:[],fontPolicy:{basis:'design',families:[font]},verifyArtifactToolImport:true,receiptPath:build+'/validation.json'});
console.log(JSON.stringify(result));
