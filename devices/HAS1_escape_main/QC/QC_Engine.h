#ifndef QC_ENGINE_H
#define QC_ENGINE_H

#include "QC_Types.h"
#include <vector>

class QCEngine {
private:
  std::vector<IQCRule *> fastRules;
  std::vector<IQCRule *> slowRules;
  unsigned long lastSlowCheckTime;
  unsigned long slowCheckInterval;

  QCEngine() : lastSlowCheckTime(0), slowCheckInterval(2000) {}

public:
  static QCEngine &getInstance() {
    static QCEngine instance;
    return instance;
  }

  QCEngine(const QCEngine &) = delete;
  void operator=(const QCEngine &) = delete;

  void begin(unsigned long intervalMs = 2000) {
    slowCheckInterval = intervalMs;
    Serial.println("[QC] Engine Initialized. Slow Interval: " +
                   String(slowCheckInterval) + "ms");
  }

  void addRule(IQCRule *rule) {
    if (rule->isFastCheck()) {
      fastRules.push_back(rule);
    } else {
      slowRules.push_back(rule);
    }
    Serial.println("[QC] Rule Added: " + rule->getId() +
                   " (" + rule->getName() + ")");
  }

  void tick() {
    for (auto rule : fastRules) {
      QCResult result = rule->check();
      if (result.isIssue()) {
        processResult(result);
      }
    }
    if (millis() - lastSlowCheckTime >= slowCheckInterval) {
      lastSlowCheckTime = millis();
      for (auto rule : slowRules) {
        QCResult result = rule->check();
        if (result.isIssue()) {
          processResult(result);
        }
      }
    }
  }

  void processResult(const QCResult &result) {
    static String recentRuleIds[16];
    static unsigned long recentRuleMs[16] = {0};
    const unsigned long now = millis();
    int slot = -1;

    for (int i = 0; i < 16; ++i) {
      if (recentRuleIds[i] == result.ruleId) {
        slot = i;
        break;
      }
      if (slot == -1 && recentRuleIds[i].length() == 0) {
        slot = i;
      }
    }
    if (slot == -1) slot = 0;

    // 동일 룰의 고빈도 반복 출력 억제 (3초 쿨다운)
    if (recentRuleIds[slot] == result.ruleId &&
        (now - recentRuleMs[slot]) < 3000UL) {
      return;
    }

    Serial.println(result.toString());
    recentRuleIds[slot] = result.ruleId;
    recentRuleMs[slot] = now;
  }
};

#endif // QC_ENGINE_H
