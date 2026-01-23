#pragma once

#include <boost/serialization/split_free.hpp>
#include <chrono>

namespace boost::serialization {

// Сохранение std::chrono::time_point
        template<class Archive, class Clock, class Duration>
        void save(Archive & ar, const std::chrono::time_point<Clock, Duration> & t, const unsigned int version)
        {
            // Преобразуем time_point в количество тиков с момента эпохи
            auto duration = t.time_since_epoch();
            auto ticks = duration.count();
            ar & ticks;
        }

// Загрузка std::chrono::time_point
        template<class Archive, class Clock, class Duration>
        void load(Archive & ar, std::chrono::time_point<Clock, Duration> & t, const unsigned int version)
        {
            typename Duration::rep ticks;
            ar & ticks;
            Duration duration(ticks);
            t = std::chrono::time_point<Clock, Duration>(duration);
        }

// Разделение функций сохранения и загрузки
        template<class Archive, class Clock, class Duration>
        void serialize(Archive & ar, std::chrono::time_point<Clock, Duration> & t, const unsigned int version)
        {
            boost::serialization::split_free(ar, t, version);
        }

} // namespace boost
