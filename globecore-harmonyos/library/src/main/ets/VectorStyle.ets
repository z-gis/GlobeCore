// VectorStyle.ets —— 矢量/叠加图层渲染样式值对象（对应 Android VectorStyle.kt）。
//
// 字段一一对应 native globecore::VectorStyle（cpp/vector/VectorGeometry.h）：
// 面填充、面描边、线、点、标注。颜色为 #AARRGGBB 打包数值（以十进制无符号写值，
// NativeLib 门面统一经 toInt32 折算为 int32 位模式传 NAPI）；线宽单位像素、点半径单位 dp。
// 默认值对齐 native VectorStyle 与原主界面（半透明蓝填充 + 白描边、橙红线/点）。
export class VectorStyle {
  // 面填充默认色：半透明蓝 #664A8FE3（十六进制字面量即无符号整数语义，勿改）
  static readonly DEFAULT_FILL_COLOR: number = 0x664A8FE3;
  // 面描边/标注默认色：白 #FFFFFFFF
  static readonly DEFAULT_OUTLINE_COLOR: number = 0xFFFFFFFF;
  // 线/点默认色：橙红 #FFE56B33
  static readonly DEFAULT_LINE_COLOR: number = 0xFFE56B33;
  // 标注轮廓默认色：黑 #FF000000
  static readonly DEFAULT_LABEL_OUTLINE_COLOR: number = 0xFF000000;

  /** 面填充色（#AARRGGBB） */
  fillColor: number;
  /** 面描边色（#AARRGGBB） */
  outlineColor: number;
  /** 面描边线宽（像素，屏幕空间等宽） */
  outlineWidth: number;
  /** 线要素色（#AARRGGBB） */
  lineColor: number;
  /** 线要素线宽（像素） */
  lineWidth: number;
  /** 点要素色（#AARRGGBB） */
  pointColor: number;
  /** 点要素屏幕固定半径（dp，不随缩放变化） */
  pointRadiusDp: number;
  /** 标注字段名：空则整层不标注（对齐主界面门控） */
  labelField: string;
  /** 标注文字色（#AARRGGBB，默认白） */
  labelColor: number;
  /** 标注字号缩放（1.0 = 基准字号） */
  labelSize: number;
  /** 标注是否描边 */
  labelOutline: boolean;
  /** 标注轮廓色（#AARRGGBB，默认黑） */
  labelOutlineColor: number;

  constructor(fillColor: number = VectorStyle.DEFAULT_FILL_COLOR,
    outlineColor: number = VectorStyle.DEFAULT_OUTLINE_COLOR,
    outlineWidth: number = 1.0,
    lineColor: number = VectorStyle.DEFAULT_LINE_COLOR,
    lineWidth: number = 2.0,
    pointColor: number = VectorStyle.DEFAULT_LINE_COLOR,
    pointRadiusDp: number = 5.0,
    labelField: string = '',
    labelColor: number = VectorStyle.DEFAULT_OUTLINE_COLOR,
    labelSize: number = 1.0,
    labelOutline: boolean = false,
    labelOutlineColor: number = VectorStyle.DEFAULT_LABEL_OUTLINE_COLOR) {
    this.fillColor = fillColor;
    this.outlineColor = outlineColor;
    this.outlineWidth = outlineWidth;
    this.lineColor = lineColor;
    this.lineWidth = lineWidth;
    this.pointColor = pointColor;
    this.pointRadiusDp = pointRadiusDp;
    this.labelField = labelField;
    this.labelColor = labelColor;
    this.labelSize = labelSize;
    this.labelOutline = labelOutline;
    this.labelOutlineColor = labelOutlineColor;
  }
}
