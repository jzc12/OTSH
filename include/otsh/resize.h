#pragma once

#include <cstddef>

namespace otsh
{

    // Tracks progress for table-level incremental migration.
    class ResizeManager
    {
    public:
        void start(size_t total_units)
        {
            total_units_ = total_units;
            progress_ = 0;
            active_ = total_units_ > 0;
        }

        void reset()
        {
            total_units_ = 0;
            progress_ = 0;
            active_ = false;
        }

        bool in_progress() const { return active_; }
        size_t progress() const { return progress_; }
        size_t total_units() const { return total_units_; }

        template <typename Fn>
        void step_budget(int units, Fn &&migrate_one)
        {
            while (active_ && units-- > 0 && progress_ < total_units_)
            {
                migrate_one(progress_);
                ++progress_;
            }
            if (progress_ >= total_units_)
                active_ = false;
        }

        template <typename Fn>
        void step_budget_checked(int units, Fn &&migrate_one)
        {
            while (active_ && units-- > 0 && progress_ < total_units_)
            {
                if (!migrate_one(progress_))
                    break;
                ++progress_;
            }
            if (progress_ >= total_units_)
                active_ = false;
        }

    private:
        size_t total_units_ = 0;
        size_t progress_ = 0;
        bool active_ = false;
    };

} // namespace otsh
