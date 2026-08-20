#ifndef QC_TYPES_H
#define QC_TYPES_H

#include <Arduino.h>

/**
 * @brief QC Severity Levels
 * FAIL: 즉시 확인이 필요한 차단 오류
 * WARN: 비차단 경고 (성능/품질 저하)
 * PASS: 정상
 */
enum class QCLevel { PASS, WARN, FAIL };

/**
 * @brief 표준화된 QC 결과 구조체
 */
struct QCResult {
  QCLevel level;
  String ruleId;    // 룰 고유 ID (예: "HW_PIN_01")
  String what;      // 점검 대상 (변수/장치명)
  String criterion; // 기준값 또는 임계값
  String value;     // 실제 측정값
  String fix;       // 권장 조치
  unsigned long timestamp;

  QCResult() : level(QCLevel::PASS), timestamp(0) {}

  QCResult(QCLevel l, String id, String w, String c, String v, String f)
      : level(l), ruleId(id), what(w), criterion(c), value(v), fix(f) {
    timestamp = millis();
  }

  bool isIssue() const { return level != QCLevel::PASS; }

  String toString() const {
    String levelStr = (level == QCLevel::FAIL) ? "[FAIL]" : "[WARN]";
    return levelStr + " [" + ruleId + "] " + what + " = " + value +
           " (Limit: " + criterion + ") -> Fix: " + fix;
  }
};

/**
 * @brief 모든 QC 룰의 추상 베이스 클래스
 */
class IQCRule {
public:
  virtual ~IQCRule() {}
  virtual String getId() const = 0;
  virtual String getName() const = 0;
  virtual QCResult check() = 0;
  // true = 매 tick 실행 (fast), false = 주기적 실행 (slow)
  virtual bool isFastCheck() const = 0;
};

#endif // QC_TYPES_H
