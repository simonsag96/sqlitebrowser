#ifndef PLOTDOCK_H
#define PLOTDOCK_H

#include "Palette.h"

#include <QColor>
#include <QDialog>
#include <QString>
#include <QVariant>
#include <QVector>

#include <map>
#include <vector>

class QMenu;
class QPrinter;
class QTreeWidgetItem;
class QCPAxis;
class QCPColorScale;
class QCPMarginGroup;
class QCPRange;
class QMouseEvent;

class SqliteTableModel;
struct BrowseDataTableSettings;

namespace Ui {
class PlotDock;
}

class PlotDock : public QDialog
{
    Q_OBJECT

public:
    explicit PlotDock(QWidget* parent = nullptr);
    ~PlotDock() override;

    struct PlotSettings
    {
        int lineStyle;
        int pointShape;
        QColor colour;
        bool active;

        PlotSettings() :
            lineStyle(0),
            pointShape(0),
            active(false)
        {}

        PlotSettings(int _lineStyle, int _pointShape, QColor _colour, bool _active) :
            lineStyle(_lineStyle),
            pointShape(_pointShape),
            colour(_colour),
            active(_active)
        {}

        friend QDataStream& operator<<(QDataStream& stream, const PlotDock::PlotSettings& object)
        {
            stream << object.lineStyle;
            stream << object.pointShape;
            stream << object.colour;
            stream << object.active;

            return stream;
        }
        friend QDataStream& operator>>(QDataStream& stream, PlotDock::PlotSettings& object)
        {
            stream >> object.lineStyle;
            stream >> object.pointShape;
            stream >> object.colour;

            if(!stream.atEnd())
                stream >> object.active;

            return stream;
        }
    };

public slots:
    void updatePlot(SqliteTableModel* model, BrowseDataTableSettings* settings = nullptr, bool update = true, bool keepOrResetSelection = true);
    void fetchAllData();
    void resetPlot();
    void reject() override;

signals:
    void pointsSelected(int firstIndex, int count);

private:
    enum PlotColumns
    {
        PlotColumnField = 0,
        PlotColumnX = 1,
        PlotColumnY1 = 2,
        PlotColumnY2 = 3,
        PlotColumnColour = 4,
        PlotColumnType = 5,
    };

    // Must match the item order of the comboPlotType combo box in PlotDock.ui
    enum PlotType
    {
        PlotTypeXY = 0,
        PlotTypeHistogram = 1,
        PlotTypeBoxPlot = 2,
    };

    Ui::PlotDock* ui;

    SqliteTableModel* m_currentPlotModel;
    BrowseDataTableSettings* m_currentTableSettings;
    QMenu* m_contextMenu;
    bool m_showLegend;
    bool m_stackedBars;
    bool m_fixedFormat;
    Palette m_graphPalette;
    std::vector<QCPAxis *> yAxes;
    std::vector<int> PlotColumnY;
    unsigned int m_xtype;
    QCPColorScale* m_colorScale;
    QCPMarginGroup* m_colorScaleMarginGroup;

    /*!
     * \brief guessdatatype try to parse the first 10 rows and decide the datatype
     * \param model model to check the data
     * \param column index of the column to check
     * \return the guessed datatype
     */
    QVariant::Type guessDataType(SqliteTableModel* model, int column) const;
    void adjustBars();
    void adjustAxisFormat();

    /*!
     * \brief Find the tree item whose given check column is checked, if any.
     * \param column one of the PlotColumns check columns (e.g. PlotColumnX, PlotColumnColour)
     * \return the checked item or nullptr when none is checked
     */
    QTreeWidgetItem* checkedItem(int column) const;

    /*!
     * \brief Draw the colour-scaled scatter points for a single y-series on top of its plottable.
     *
     * The points are coloured according to the value of the selected colour column, mapped through
     * the currently selected gradient. The overlay is purely visual and not selectable, so the
     * existing point/row selection mechanism on the underlying plottable keeps working.
     * \param valueAxis the y axis the series is attached to
     * \param xdata the already computed x coordinates of the series
     * \param ydata the y coordinates of the series
     * \param colorData the value of the colour column for each point
     * \param range the value range used to map colours
     * \param shape the scatter shape to use for the points
     */
    void drawColorScaledPoints(QCPAxis* valueAxis, const QVector<double>& xdata, const QVector<double>& ydata,
                               const QVector<double>& colorData, const QCPRange& range, int shape);

    /*!
     * \brief Draw categorically-coloured scatter points for a single y-series on top of its plottable.
     *
     * Used when the colour column is a label column: every distinct label gets its own colour and,
     * optionally, a legend entry. Like \ref drawColorScaledPoints the overlay is purely decorative
     * and not selectable.
     * \param valueAxis the y axis the series is attached to
     * \param xdata the already computed x coordinates of the series
     * \param ydata the y coordinates of the series
     * \param rowLabels the label of the colour column for each point (null string for NULL cells)
     * \param labelColors mapping of each distinct label to its colour
     * \param shape the scatter shape to use for the points
     * \param addToLegend whether the created overlay graphs should appear in the legend
     */
    void drawCategoricalPoints(QCPAxis* valueAxis, const QVector<double>& xdata, const QVector<double>& ydata,
                               const QVector<QString>& rowLabels, const std::map<QString, QColor>& labelColors,
                               int shape, bool addToLegend);

    /*!
     * \brief Draw a histogram of a single numeric (Y) column.
     *
     * Uses the first checked numeric Y column. The number of bins is taken from the Bins spin box,
     * or computed automatically (Sturges' rule) when that is set to 0/Auto.
     */
    void drawHistogram();

    /*!
     * \brief Draw a box-and-whisker plot of the numeric (Y) column(s) grouped by the label (X) column.
     *
     * Requires the X column to be a label/string column and at least one numeric Y column. One box is
     * drawn per distinct X value; with several Y columns the boxes are grouped side by side per category.
     */
    void drawBoxPlot();

    /*!
     * \brief Show only the toolbar controls relevant for the given plot type.
     */
    void updatePlotControlsVisibility(int plotType);

private slots:
    void columnItemChanged(QTreeWidgetItem* item, int column);
    void columnItemDoubleClicked(QTreeWidgetItem* item, int column);
    void savePlot();
    void lineTypeChanged(int index);
    void pointShapeChanged(int index);
    void colorGradientChanged(int index);
    void plotTypeChanged(int index);
    void histogramBinsChanged(int bins);
    void selectionChanged();
    void mousePress();
    void mouseWheel();
    void mouseMove(QMouseEvent* event);
    void copy();
    void toggleLegendVisible(bool visible);
    void toggleStackedBars(bool stacked);
    void openPrintDialog();
    void renderPlot(QPrinter* printer);
};

#endif
