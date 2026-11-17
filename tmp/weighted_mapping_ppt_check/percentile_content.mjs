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
s=slide('修正結果：門檻與加權使用同一份分布',3,
 '目前 Enhancement/AGC/agc_weighted.c:345–354，processed_pixels=sum(scratch)，rank=(processed_pixels-1)*bright_percentile_tenths，order_dn(scratch,... )，其後用同一scratch做interval_weight。門檻與權重在每張影格以處理後分布重算；分塊實作應各自依tile處理後分布重算。本例interval midpoints250/350的舊threshold220會觸發Bright Weight，新threshold400不觸發；midpoint450則兩者都觸發。P值未改，DN門檻改變。修正保證計算來源與流程一致，不宣稱所有閃爍或感測器雜訊因此消除。');
text(s,'P80 的設定不變；改的是計算門檻所使用的資料',70,151,1140,70,33,accent,true);
table(s,[['比較項目','原本','修正後'],['P80 的資料來源','輸入直方圖','Clip／Redistribution 後'],['範例 P80 門檻','220 DN','400 DN'],['250／350 DN 區間中點','會被 Bright Weight 加權','不會被 Bright Weight 加權']],260,284,[390,350,400]);
text(s,'避免用舊門檻錯分亮部範圍；加權與後續映射採用一致的分布',70,577,1140,65,28);
text(s,'這是百分位計算順序的修正；不等於降噪，也不保證所有亮度跳動都消失',70,640,1140,40,22,muted);
for(let i=0;i<deck.slides.items.length;i++){
 const blob=await deck.export({slide:deck.slides.items[i],format:'png',scale:1});
 await fs.writeFile(`${build}/percentile-slide-${i+1}.png`,new Uint8Array(await blob.arrayBuffer()));
}
const draft=build+'/percentile_candidate.pptx';await(await PresentationFile.exportPptx(deck)).save(draft);
const {finalizePresentation}=await import(pathToFileURL(skill+'/container_tools/artifact_tool_utils.mjs').href);
const result=await finalizePresentation({workspaceDir:work,candidatePath:draft,finalPath:work+'/output/WeightedMapping_Percentile修正說明.pptx',pythonExecutable:'C:/Users/pega_user/AppData/Local/Programs/Python/Python313/python.exe',integrityValidatorPath:skill+'/container_tools/inspect_presentation_package_integrity.py',layoutValidatorPath:skill+'/container_tools/inspect_presentation_layout_geometry.py',layoutArgs:['--expected-slide-size-emu','12192000,6858000','--validate-heading-fit','--require-native-table-slide','2','--require-native-table-slide','3'],explicitTotalSlideCount:3,requiredNativeTableOwnerSlides:[2,3],requiredNativeChartOwnerSlides:[],fontPolicy:{basis:'design',families:[font]},verifyArtifactToolImport:true,receiptPath:build+'/percentile_validation.json'});
console.log(result.finalPath);
