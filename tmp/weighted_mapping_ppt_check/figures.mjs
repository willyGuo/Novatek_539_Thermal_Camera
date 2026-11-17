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
